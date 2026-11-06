#include <stackide/base/log.hpp>
#include <stackide/profile/stall_detector.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <string_view>
#include <thread>

using namespace std::chrono_literals;
using Catch::Matchers::ContainsSubstring;
using stackide::StallDetector;

TEST_CASE("an injected 50 ms stall is reported with its location")
{
    stackide::LogConfig config;
    config.to_stderr = false;
    const stackide::LogSession session{config};

    StallDetector detector{10.0};
    detector.begin_iteration();
    detector.phase("events");
    detector.phase("slow part");
    const unsigned line = __LINE__ - 1;
    std::this_thread::sleep_for(50ms);
    detector.phase("render");
    const auto stall = detector.end_iteration();

    REQUIRE(stall);
    CHECK(stall->total_ms >= 50.0);
    CHECK(stall->longest_phase_ms >= 50.0);
    CHECK(std::string_view{stall->longest_phase} == "slow part");
    CHECK(std::string_view{stall->file}.ends_with("test_stall_detector.cpp"));
    CHECK(stall->line == line);

    stackide::log_flush();
    const auto history = stackide::log_recent_history();
    CHECK_THAT(history, ContainsSubstring("main loop stall"));
    CHECK_THAT(history, ContainsSubstring("slow part"));
    CHECK_THAT(history, ContainsSubstring("test_stall_detector.cpp"));
}

TEST_CASE("an iteration within budget reports nothing")
{
    StallDetector detector{10'000.0};
    detector.begin_iteration();
    detector.phase("quick");
    CHECK_FALSE(detector.end_iteration());
}

TEST_CASE("paused time doesn't count")
{
    StallDetector detector{20.0};
    detector.begin_iteration();
    detector.phase("before");
    detector.pause();
    std::this_thread::sleep_for(50ms);
    detector.phase("after");
    CHECK_FALSE(detector.end_iteration());
}

TEST_CASE("each iteration starts fresh")
{
    StallDetector detector{10.0};
    detector.begin_iteration();
    detector.phase("slow");
    std::this_thread::sleep_for(20ms);
    REQUIRE(detector.end_iteration());

    detector.begin_iteration();
    detector.phase("quick");
    detector.set_budget_ms(10'000.0);
    CHECK_FALSE(detector.end_iteration());
}
