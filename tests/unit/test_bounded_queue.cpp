#include <stackide/base/bounded_queue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

using stackide::BoundedQueue;

TEST_CASE("items come out in the order they went in")
{
    BoundedQueue<int> queue{3};
    CHECK(queue.try_push(1));
    CHECK(queue.try_push(2));
    CHECK(queue.try_pop() == 1);
    CHECK(queue.try_push(3));
    CHECK(queue.try_push(4));
    CHECK(queue.try_pop() == 2);
    CHECK(queue.try_pop() == 3);
    CHECK(queue.try_pop() == 4);
    CHECK_FALSE(queue.try_pop());
}

TEST_CASE("try_push fails on a full queue without taking the value")
{
    BoundedQueue<std::unique_ptr<int>> queue{1};
    CHECK(queue.try_push(std::make_unique<int>(1)));
    auto second = std::make_unique<int>(2);
    CHECK_FALSE(queue.try_push(std::move(second)));
    REQUIRE(second != nullptr);
    CHECK(*second == 2);
}

TEST_CASE("maybe_has_items follows the contents")
{
    BoundedQueue<int> queue{2};
    CHECK_FALSE(queue.maybe_has_items());
    REQUIRE(queue.try_push(1));
    CHECK(queue.maybe_has_items());
    REQUIRE(queue.try_pop());
    CHECK_FALSE(queue.maybe_has_items());
}

TEST_CASE("a blocked push is released by stop, and by space")
{
    BoundedQueue<int> queue{1};
    REQUIRE(queue.try_push(1));

    std::atomic<int> result{-1};
    {
        std::jthread producer{[&](const std::stop_token& stop) { result = queue.push(2, stop) ? 1 : 0; }};
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        CHECK(result == -1);
        producer.request_stop();
    }
    CHECK(result == 0);

    {
        std::jthread producer{[&](const std::stop_token& stop) { result = queue.push(3, stop) ? 1 : 0; }};
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        std::optional<int> first;
        while (!first)
        {
            first = queue.try_pop();
        }
        CHECK(first == 1);
    }
    CHECK(result == 1);
    CHECK(queue.try_pop() == 3);
}

TEST_CASE("a blocked pop is released by stop, and by an item")
{
    BoundedQueue<int> queue{1};
    std::jthread stopped{[&](const std::stop_token& stop) { CHECK_FALSE(queue.pop(stop)); }};
    stopped.request_stop();
    stopped.join();

    std::atomic<int> received{0};
    std::jthread consumer{[&](const std::stop_token& stop) { received = queue.pop(stop).value_or(-1); }};
    while (!queue.try_push(7))
    {
    }
    consumer.join();
    CHECK(received == 7);
}

TEST_CASE("wakes are coalesced until cleared")
{
    int wakes = 0;
    BoundedQueue<int> queue{8, [&] { ++wakes; }};

    REQUIRE(queue.try_push(1));
    REQUIRE(queue.try_push(2));
    CHECK(wakes == 1);

    queue.clear_wake();
    CHECK(wakes == 1);
    REQUIRE(queue.try_push(3));
    CHECK(wakes == 2);
}

// The stress test #144 asks for, with a small capacity so that producers block constantly.
// Every value has to arrive exactly once and in order per producer. Between drains the
// consumer listens only for a wake, as a sleeping main loop does, so a lost wake fails here.
TEST_CASE("eight producers deliver everything to one consumer")
{
    constexpr std::size_t kProducers = 8;
    constexpr std::uint64_t kItemsEach = 20'000;
    constexpr auto kWakeLimit = std::chrono::seconds{30};

    std::atomic<std::uint64_t> wakes{0};
    BoundedQueue<std::pair<std::size_t, std::uint64_t>> queue{16, [&] { wakes.fetch_add(1); }};

    std::vector<std::jthread> producers;
    for (std::size_t p = 0; p < kProducers; ++p)
    {
        producers.emplace_back(
            [p, &queue](const std::stop_token& stop)
            {
                for (std::uint64_t i = 0; i < kItemsEach && !stop.stop_requested(); ++i)
                {
                    queue.push({p, i}, stop);
                }
            });
    }

    std::array<std::uint64_t, kProducers> next{};
    std::uint64_t received = 0;
    while (received < kProducers * kItemsEach)
    {
        const std::uint64_t seen_wakes = wakes.load();
        queue.clear_wake();
        while (auto item = queue.try_pop())
        {
            REQUIRE(item->second == next.at(item->first));
            ++next.at(item->first);
            ++received;
        }

        // No more wakes are coming once everything has arrived.
        if (received < kProducers * kItemsEach && !queue.maybe_has_items())
        {
            const auto deadline = std::chrono::steady_clock::now() + kWakeLimit;
            while (wakes.load() == seen_wakes)
            {
                if (std::chrono::steady_clock::now() > deadline)
                {
                    for (auto& producer : producers)
                    {
                        producer.request_stop();
                    }
                    FAIL("a wake was lost with " << received << " items received");
                }
                std::this_thread::yield();
            }
        }
    }
    CHECK_FALSE(queue.try_pop());
}
