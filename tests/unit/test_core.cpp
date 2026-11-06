#include <stackide/core/core.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <tuple>
#include <variant>
#include <vector>

using namespace stackide;
using namespace std::chrono_literals;

namespace
{

    ProgressToken start_retrying(Core& core, const std::string& title, bool cancellable, const Core::ProgressTask& task)
    {
        while (true)
        {
            if (auto token = core.start_progress(Lane::Background, title, cancellable, task))
            {
                return *token;
            }
            std::this_thread::yield();
        }
    }

    // Reads messages until the given task's ProgressEnd, as the UI would.
    std::vector<CoreToUi> collect_until_end(Core& core, ProgressToken token)
    {
        std::vector<CoreToUi> messages;
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (auto message = core.to_ui().try_pop())
            {
                messages.push_back(*message);
                if (const auto* end = std::get_if<ProgressEnd>(&*message); end != nullptr && end->token == token)
                {
                    return messages;
                }
            }
            else
            {
                std::this_thread::sleep_for(100us);
            }
        }
        FAIL("the task never reported ProgressEnd");
        return messages;
    }

} // namespace

TEST_CASE("a task reports begin, progress and end in order")
{
    Core core{[] {}};
    const auto token = start_retrying(core, "indexing", false,
                                      [](const std::stop_token&, ProgressReporter& progress)
                                      {
                                          progress.report("half", std::uint8_t{50});
                                          progress.report("too far", std::uint8_t{200});
                                      });

    const auto messages = collect_until_end(core, token);
    REQUIRE(messages.size() == 4);
    const auto& begin = std::get<ProgressBegin>(messages.at(0));
    CHECK(begin.token == token);
    CHECK(begin.title == "indexing");
    CHECK_FALSE(begin.cancellable);
    CHECK(std::get<ProgressReport>(messages.at(1)).percentage == 50);
    CHECK(std::get<ProgressReport>(messages.at(2)).percentage == 100);
    CHECK(std::get<ProgressEnd>(messages.at(3)).message.empty());
}

TEST_CASE("a cancel message stops a cancellable task, which still ends")
{
    std::atomic<bool> started{false};
    Core core{[] {}};
    const auto token = start_retrying(core, "long", true,
                                      [&](const std::stop_token& stop, ProgressReporter&)
                                      {
                                          started = true;
                                          while (!stop.stop_requested())
                                          {
                                              std::this_thread::sleep_for(1ms);
                                          }
                                      });
    while (!started)
    {
        std::this_thread::yield();
    }
    while (!core.send(CancelProgress{.token = token}))
    {
        std::this_thread::yield();
    }

    const auto messages = collect_until_end(core, token);
    CHECK(std::get<ProgressEnd>(messages.back()).message == "cancelled");
}

TEST_CASE("a shutdown message stops every cancellable task")
{
    Core core{[] {}};
    const Core::ProgressTask task = [](const std::stop_token& stop, ProgressReporter&)
    {
        while (!stop.stop_requested())
        {
            std::this_thread::sleep_for(1ms);
        }
    };
    const auto first = start_retrying(core, "first", true, task);
    const auto second = start_retrying(core, "second", true, task);
    while (!core.send(ShutdownRequested{}))
    {
        std::this_thread::yield();
    }
    bool first_ended = false;
    bool second_ended = false;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!(first_ended && second_ended) && std::chrono::steady_clock::now() < deadline)
    {
        if (auto message = core.to_ui().try_pop())
        {
            if (const auto* end = std::get_if<ProgressEnd>(&*message))
            {
                first_ended = first_ended || end->token == first;
                second_ended = second_ended || end->token == second;
            }
        }
    }
    CHECK(first_ended);
    CHECK(second_ended);
}

TEST_CASE("a failing task still ends")
{
    Core core{[] {}};
    const auto token = start_retrying(core, "doomed", false, [](const std::stop_token&, ProgressReporter&)
                                      { throw std::runtime_error{"expected in this test"}; });
    const auto messages = collect_until_end(core, token);
    CHECK(std::get<ProgressEnd>(messages.back()).message == "failed");
}

TEST_CASE("cancelling a finished or unknown task is harmless")
{
    Core core{[] {}};
    const auto token = start_retrying(core, "quick", true, [](const std::stop_token&, ProgressReporter&) {});
    const auto messages = collect_until_end(core, token);
    CHECK(std::get<ProgressEnd>(messages.back()).message.empty());
    while (!core.send(CancelProgress{.token = token}))
    {
        std::this_thread::yield();
    }
    while (!core.send(CancelProgress{.token = ProgressToken{.value = 999'999}}))
    {
        std::this_thread::yield();
    }
}

TEST_CASE("the wake callback fires when a message arrives")
{
    std::atomic<int> wakes{0};
    Core core{[&] { wakes.fetch_add(1); }};
    const auto token = start_retrying(core, "wake", false, [](const std::stop_token&, ProgressReporter&) {});
    collect_until_end(core, token);
    CHECK(wakes.load() > 0);
}

TEST_CASE("destroying the core releases a task blocked on a full queue")
{
    CoreConfig config;
    config.message_capacity = 1;
    Core core{[] {}, config};
    std::ignore = start_retrying(core, "chatty", true,
                                 [](const std::stop_token& stop, ProgressReporter& progress)
                                 {
                                     while (!stop.stop_requested())
                                     {
                                         progress.report("nobody is reading this");
                                     }
                                 });
    std::this_thread::sleep_for(10ms);
    SUCCEED();
}
