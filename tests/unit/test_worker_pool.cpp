#include <stackide/core/worker_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <latch>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <stop_token>
#include <thread>

using namespace stackide;
using namespace std::chrono_literals;

namespace
{

    // A spawn can fail because the queue's lock is busy, which is expected and retryable.
    TaskHandle spawn_retrying(WorkerPool::Scheduler scheduler, const WorkerPool::Task& task)
    {
        while (true)
        {
            if (auto handle = scheduler.spawn(task))
            {
                return *handle;
            }
            std::this_thread::yield();
        }
    }

    bool wait_for(const std::atomic<bool>& flag, std::chrono::milliseconds limit)
    {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (!flag.load() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(1ms);
        }
        return flag.load();
    }

    WorkerPoolConfig small_pool()
    {
        WorkerPoolConfig config;
        config.interactive_threads = 1;
        config.background_threads = 2;
        return config;
    }

} // namespace

TEST_CASE("tasks run on both lanes")
{
    // In every test here, whatever a task touches is declared before the pool, so that it
    // outlives the threads the pool joins on destruction.
    std::latch done{2};
    WorkerPool pool{small_pool()};
    spawn_retrying(pool.scheduler(Lane::Interactive), [&](const std::stop_token&) { done.count_down(); });
    spawn_retrying(pool.scheduler(Lane::Background), [&](const std::stop_token&) { done.count_down(); });
    done.wait();
    SUCCEED();
}

// Every background thread is held and its queue is full, so an interactive task can only
// run if it really has threads of its own.
TEST_CASE("a background flood doesn't delay interactive work")
{
    std::binary_semaphore release{0};
    std::latch running{2};
    std::atomic<bool> interactive_ran{false};
    WorkerPool pool{small_pool()};
    const WorkerPool::Task blocker = [&](const std::stop_token&)
    {
        running.count_down();
        release.acquire();
        release.release();
    };
    spawn_retrying(pool.scheduler(Lane::Background), blocker);
    spawn_retrying(pool.scheduler(Lane::Background), blocker);
    running.wait();

    int queued = 0;
    while (pool.scheduler(Lane::Background).spawn([](const std::stop_token&) {}))
    {
        ++queued;
    }
    CHECK(queued > 1000);

    spawn_retrying(pool.scheduler(Lane::Interactive), [&](const std::stop_token&) { interactive_ran = true; });
    const bool ran = wait_for(interactive_ran, 5s);
    release.release();
    CHECK(ran);
}

TEST_CASE("a task can be stopped through its handle")
{
    std::atomic<bool> started{false};
    std::atomic<bool> saw_stop{false};
    WorkerPool pool{small_pool()};
    TaskHandle handle = spawn_retrying(pool.scheduler(Lane::Background),
                                       [&](const std::stop_token& stop)
                                       {
                                           started = true;
                                           while (!stop.stop_requested())
                                           {
                                               std::this_thread::sleep_for(1ms);
                                           }
                                           saw_stop = true;
                                       });
    REQUIRE(wait_for(started, 5s));
    handle.request_stop();
    CHECK(wait_for(saw_stop, 5s));
}

TEST_CASE("destroying the pool stops running tasks")
{
    std::atomic<bool> started{false};
    std::atomic<bool> saw_stop{false};
    {
        WorkerPool pool{small_pool()};
        spawn_retrying(pool.scheduler(Lane::Background),
                       [&](const std::stop_token& stop)
                       {
                           started = true;
                           while (!stop.stop_requested())
                           {
                               std::this_thread::sleep_for(1ms);
                           }
                           saw_stop = true;
                       });
        REQUIRE(wait_for(started, 5s));
    }
    CHECK(saw_stop);
}

TEST_CASE("an exception from a task doesn't take the worker down")
{
    WorkerPoolConfig config = small_pool();
    config.background_threads = 1;
    std::atomic<bool> next_ran{false};
    WorkerPool pool{config};
    spawn_retrying(pool.scheduler(Lane::Background),
                   [](const std::stop_token&) { throw std::runtime_error{"expected in this test"}; });

    spawn_retrying(pool.scheduler(Lane::Background), [&](const std::stop_token&) { next_ran = true; });
    CHECK(wait_for(next_ran, 5s));
}
