#include <stackide/core/core.hpp>
#include <stackide/platform/main_loop.hpp>
#include <stackide/platform/video.hpp>

#include <catch2/catch_test_macros.hpp>

#include <SDL3/SDL.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace stackide;
using namespace std::chrono_literals;

namespace
{

    CoreToUi numbered_message(std::size_t number)
    {
        return ProgressEnd{.token = ProgressToken{.value = number}, .message = ""};
    }

    std::size_t number_of(const CoreToUi& message) { return std::get<ProgressEnd>(message).token.value; }

    void push_quit_event()
    {
        SDL_Event event{};
        event.type = SDL_EVENT_QUIT;
        REQUIRE(SDL_PushEvent(&event));
    }

    void drain_os_events() { SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST); }

} // namespace

TEST_CASE("the wakeup event type is registered")
{
    const auto video = platform::Video::start(platform::VideoBackend::Dummy);
    REQUIRE(video);
    CHECK(platform::wakeup_event_type() != 0);
}

TEST_CASE("OS events are handled before messages, each source in order")
{
    const auto video = platform::Video::start(platform::VideoBackend::Dummy);
    REQUIRE(video);
    drain_os_events();

    BoundedQueue<CoreToUi> queue{16};
    platform::MainLoop loop{queue};

    // The messages are queued first, so only the loop's ordering can put the event ahead.
    REQUIRE(queue.try_push(numbered_message(1)));
    REQUIRE(queue.try_push(numbered_message(2)));
    push_quit_event();

    std::vector<std::string> order;
    loop.pump([&](const platform::Event& event)
              { order.emplace_back(event.type == platform::EventType::Quit ? "quit" : "other"); },
              [&](CoreToUi&& message) { order.push_back(std::to_string(number_of(message))); });

    CHECK(order == std::vector<std::string>{"quit", "1", "2"});
}

TEST_CASE("a message flood is split across pumps, and wait doesn't sleep through the rest")
{
    const auto video = platform::Video::start(platform::VideoBackend::Dummy);
    REQUIRE(video);
    drain_os_events();

    const std::size_t total = platform::MainLoop::kMaxMessagesPerPump + 10;
    BoundedQueue<CoreToUi> queue{total};
    platform::MainLoop loop{queue};
    for (std::size_t i = 0; i < total; ++i)
    {
        REQUIRE(queue.try_push(numbered_message(i)));
    }

    std::size_t handled = 0;
    const auto count = [&](CoreToUi&&) { ++handled; };
    const auto ignore_event = [](const platform::Event&) {};

    loop.pump(ignore_event, count);
    CHECK(handled == platform::MainLoop::kMaxMessagesPerPump);

    const auto before_wait = std::chrono::steady_clock::now();
    loop.wait(5000);
    CHECK(std::chrono::steady_clock::now() - before_wait < 1s);

    loop.pump(ignore_event, count);
    CHECK(handled == total);
}

TEST_CASE("wait sleeps until the timeout when nothing happens")
{
    const auto video = platform::Video::start(platform::VideoBackend::Dummy);
    REQUIRE(video);
    drain_os_events();

    BoundedQueue<CoreToUi> queue{4};
    platform::MainLoop loop{queue};
    const auto start = std::chrono::steady_clock::now();
    loop.wait(50);
    CHECK(std::chrono::steady_clock::now() - start >= 45ms);
}

// The worker has to wake the main thread out of a long sleep through the real mechanism.
TEST_CASE("a message from a worker wakes a sleeping main loop")
{
    const auto video = platform::Video::start(platform::VideoBackend::Dummy);
    REQUIRE(video);
    drain_os_events();

    Core core{[] { platform::post_wakeup(); }};
    platform::MainLoop loop{core.to_ui()};

    std::optional<ProgressToken> token;
    while (!token)
    {
        token = core.start_progress(Lane::Background, "wake test", false, [](const std::stop_token&, ProgressReporter&)
                                    { std::this_thread::sleep_for(30ms); });
    }

    const auto start = std::chrono::steady_clock::now();
    bool ended = false;
    while (!ended && std::chrono::steady_clock::now() - start < 10s)
    {
        loop.wait(60'000);
        loop.pump([](const platform::Event&) {},
                  [&](CoreToUi&& message) { ended = ended || std::holds_alternative<ProgressEnd>(message); });
    }
    CHECK(ended);
    CHECK(std::chrono::steady_clock::now() - start < 10s);
}
