#pragma once

#include <stackide/base/bounded_queue.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

namespace stackide
{

    enum class Lane : std::uint8_t
    {
        Interactive, // the user is waiting: must not be delayed by background work
        Background,
    };

    inline constexpr std::size_t kDefaultLaneCapacity = 4096;

    struct WorkerPoolConfig
    {
        std::size_t interactive_threads = 2;
        std::size_t background_threads = 0; // 0 derives it from the core count
        std::size_t queue_capacity = kDefaultLaneCapacity; // per lane
    };

    // Stops one task. A task that hasn't started still runs, with its token already set,
    // so that whatever it reports when it finishes still gets reported.
    class TaskHandle
    {
    public:
        explicit TaskHandle(std::stop_source source) noexcept : source_(std::move(source)) {}

        void request_stop() noexcept { source_.request_stop(); }

    private:
        std::stop_source source_;
    };

    // Each lane has its own threads, so that a background flood can't occupy the threads
    // interactive work needs. It can still compete for CPU time.
    //
    // Shaped like C++26 senders and receivers, so that this can move to std::execution
    // once GCC's library support is complete. scheduler() corresponds to a scheduler,
    // spawn() to std::execution::spawn on that scheduler, and the task's stop_token to
    // get_stop_token on its environment. It's only that shape: nothing here composes.
    class WorkerPool
    {
    public:
        using Task = std::function<void(const std::stop_token&)>;

        class Scheduler
        {
        public:
            // Never waits. Empty if the lane's queue is full or busy right now, in which
            // case the caller may try again later. Exceptions are logged, not propagated.
            [[nodiscard]] std::optional<TaskHandle> spawn(Task task) const;

        private:
            friend class WorkerPool;
            Scheduler(WorkerPool& pool, Lane lane) noexcept : pool_(&pool), lane_(lane) {}

            WorkerPool* pool_;
            Lane lane_;
        };

        explicit WorkerPool(const WorkerPoolConfig& config = {});
        WorkerPool(const WorkerPool&) = delete;
        WorkerPool& operator=(const WorkerPool&) = delete;
        WorkerPool(WorkerPool&&) = delete;
        WorkerPool& operator=(WorkerPool&&) = delete;
        // Stops running tasks through their tokens and joins. Queued tasks don't run.
        ~WorkerPool();

        [[nodiscard]] Scheduler scheduler(Lane lane) noexcept { return Scheduler{*this, lane}; }

    private:
        struct Item
        {
            Task task;
            std::stop_source source;
        };

        BoundedQueue<Item>& queue_for(Lane lane) noexcept;
        void start_threads(std::size_t count, BoundedQueue<Item>& queue);

        BoundedQueue<Item> interactive_;
        BoundedQueue<Item> background_;
        std::vector<std::jthread> threads_;
    };

} // namespace stackide
