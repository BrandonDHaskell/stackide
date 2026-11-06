#pragma once

#include <stackide/base/bounded_queue.hpp>
#include <stackide/core/messages.hpp>
#include <stackide/core/worker_pool.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>

namespace stackide
{

    // What a long task uses to report on itself. Blocks if the UI has fallen far behind,
    // which slows the task down rather than growing a queue without limit.
    class ProgressReporter
    {
    public:
        ProgressReporter(BoundedQueue<CoreToUi>& to_ui, ProgressToken token, std::stop_token shutdown) noexcept :
            to_ui_(&to_ui), token_(token), shutdown_(std::move(shutdown))
        {
        }

        // percentage is clamped to 100.
        void report(std::string message, std::optional<std::uint8_t> percentage = std::nullopt);

    private:
        BoundedQueue<CoreToUi>* to_ui_;
        ProgressToken token_;
        std::stop_token shutdown_;
    };

    inline constexpr std::size_t kDefaultMessageCapacity = 1024;

    struct CoreConfig
    {
        WorkerPoolConfig workers;
        std::size_t message_capacity = kDefaultMessageCapacity;
    };

    // The core layer from docs/adr/0001-ui-core-split.md: everything the UI reaches only
    // through typed messages. on_wake runs on any thread when messages arrive for the UI,
    // and must not call back into this object.
    class Core
    {
    public:
        using ProgressTask = std::function<void(const std::stop_token&, ProgressReporter&)>;

        explicit Core(std::function<void()> on_wake, const CoreConfig& config = {});
        Core(const Core&) = delete;
        Core& operator=(const Core&) = delete;
        Core(Core&&) = delete;
        Core& operator=(Core&&) = delete;
        ~Core();

        [[nodiscard]] BoundedQueue<CoreToUi>& to_ui() noexcept { return to_ui_; }

        // Never waits, so it's safe on the main thread. False if the queue is full or busy
        // right now, in which case the message wasn't sent and should be sent again later.
        [[nodiscard]] bool send(const UiToCore& message);

        // Starts a task that always ends with ProgressEnd, including when it's cancelled or
        // throws, so that the UI can't be left showing progress forever. Never waits. Empty
        // if the lane is full or busy right now.
        [[nodiscard]] std::optional<ProgressToken> start_progress(Lane lane, std::string title, bool cancellable,
                                                                  ProgressTask task);

        [[nodiscard]] WorkerPool& workers() noexcept { return workers_; }

    private:
        void run_messages(const std::stop_token& stop);
        void handle(const UiToCore& message);
        void forget_task(ProgressToken token);

        BoundedQueue<CoreToUi> to_ui_;
        BoundedQueue<UiToCore> to_core_;
        std::stop_source shutdown_;

        std::mutex tasks_mutex_;
        std::unordered_map<std::uint64_t, std::optional<TaskHandle>> cancellable_tasks_;
        std::uint64_t next_token_ = 1;

        // Destroyed first, so that nothing running can reach the members above once gone.
        WorkerPool workers_;
        std::jthread message_thread_;
    };

} // namespace stackide
