#include <stackide/core/worker_pool.hpp>

#include <stackide/base/log.hpp>
#include <stackide/profile/profile.hpp>

#include <algorithm>
#include <exception>
#include <utility>

namespace stackide
{
    namespace
    {

        // hardware_concurrency counts SMT threads, so half is an estimate of physical
        // cores. Keeps one core back for the main thread and one for interactive work.
        std::size_t default_background_threads()
        {
            const std::size_t physical_estimate = std::thread::hardware_concurrency() / 2;
            return std::max<std::size_t>(physical_estimate, 3) - 2;
        }

        void run_task(const WorkerPool::Task& task, const std::stop_token& token)
        {
            STACKIDE_ZONE("worker task");
            try
            {
                task(token);
            }
            catch (const std::exception& error)
            {
                STACKIDE_LOG_ERROR(LogCategory::Core, "task failed: {}", error.what());
            }
            catch (...)
            {
                STACKIDE_LOG_ERROR(LogCategory::Core, "task failed with an unknown exception");
            }
        }

    } // namespace

    std::optional<TaskHandle> WorkerPool::Scheduler::spawn(Task task) const
    {
        Item item{.task = std::move(task), .source = std::stop_source{}};
        TaskHandle handle{item.source};
        if (!pool_->queue_for(lane_).try_push(std::move(item)))
        {
            return std::nullopt;
        }
        return handle;
    }

    WorkerPool::WorkerPool(const WorkerPoolConfig& config) :
        interactive_(config.queue_capacity), background_(config.queue_capacity)
    {
        start_threads(config.interactive_threads, interactive_, "interactive worker");
        start_threads(config.background_threads != 0 ? config.background_threads : default_background_threads(),
                      background_, "background worker");
    }

    WorkerPool::~WorkerPool()
    {
        for (auto& thread : threads_)
        {
            thread.request_stop();
        }
    }

    BoundedQueue<WorkerPool::Item>& WorkerPool::queue_for(Lane lane) noexcept
    {
        return lane == Lane::Interactive ? interactive_ : background_;
    }

    void WorkerPool::start_threads(std::size_t count, BoundedQueue<Item>& queue, const char* thread_name)
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            threads_.emplace_back(
                [&queue, thread_name](const std::stop_token& thread_stop)
                {
                    static_cast<void>(thread_name); // unused when profiling is compiled out
                    STACKIDE_THREAD_NAME(thread_name);
                    while (std::optional<Item> item = queue.pop(thread_stop))
                    {
                        // Shutting the pool down has to stop a running task too.
                        const std::stop_callback forward{thread_stop, [&item] { item->source.request_stop(); }};
                        run_task(item->task, item->source.get_token());
                    }
                });
        }
    }

} // namespace stackide
