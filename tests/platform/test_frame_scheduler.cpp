#include <stackide/platform/frame_scheduler.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>

using stackide::platform::FrameScheduler;

namespace
{

    constexpr std::uint64_t kMs = 1'000'000;

    constexpr double kFallbackMs = 16.0;

} // namespace

TEST_CASE("nothing is due until something asks")
{
    const FrameScheduler scheduler{kFallbackMs};
    CHECK_FALSE(scheduler.frame_due(100 * kMs));
    CHECK_FALSE(scheduler.ns_until_due(100 * kMs));
}

TEST_CASE("a redraw request is due at once, and a frame satisfies it")
{
    FrameScheduler scheduler{kFallbackMs};
    scheduler.request_redraw(10 * kMs);
    CHECK(scheduler.frame_due(10 * kMs));
    CHECK(scheduler.ns_until_due(10 * kMs) == 0U);

    scheduler.begin_frame(11 * kMs);
    CHECK_FALSE(scheduler.frame_due(11 * kMs));
}

TEST_CASE("a request made during a frame needs another frame")
{
    FrameScheduler scheduler{kFallbackMs};
    scheduler.request_redraw(0);
    scheduler.begin_frame(1 * kMs);
    scheduler.request_redraw(2 * kMs);
    scheduler.end_frame(3 * kMs);
    CHECK(scheduler.frame_due(3 * kMs));
}

TEST_CASE("a timed request isn't due until its time")
{
    FrameScheduler scheduler{kFallbackMs};
    REQUIRE(scheduler.request_redraw_at(50 * kMs));
    CHECK_FALSE(scheduler.frame_due(49 * kMs));
    CHECK(scheduler.ns_until_due(40 * kMs) == 10 * kMs);
    CHECK(scheduler.frame_due(50 * kMs));
}

TEST_CASE("timed requests fire separately, earliest first")
{
    FrameScheduler scheduler{kFallbackMs};
    REQUIRE(scheduler.request_redraw_at(90 * kMs));
    REQUIRE(scheduler.request_redraw_at(30 * kMs));
    CHECK(scheduler.ns_until_due(0) == 30 * kMs);

    scheduler.begin_frame(30 * kMs);
    CHECK_FALSE(scheduler.frame_due(30 * kMs));
    CHECK(scheduler.timed_requests_pending() == 1);
    CHECK(scheduler.ns_until_due(30 * kMs) == 60 * kMs);
}

TEST_CASE("a frame satisfies every timed request that has come due")
{
    FrameScheduler scheduler{kFallbackMs};
    REQUIRE(scheduler.request_redraw_at(10 * kMs));
    REQUIRE(scheduler.request_redraw_at(20 * kMs));
    scheduler.begin_frame(25 * kMs);
    CHECK(scheduler.timed_requests_pending() == 0);
    CHECK_FALSE(scheduler.ns_until_due(25 * kMs));
}

TEST_CASE("too many timed requests are refused, and existing ones survive")
{
    FrameScheduler scheduler{kFallbackMs};
    for (std::size_t i = 0; i < FrameScheduler::kMaxTimedRequests; ++i)
    {
        REQUIRE(scheduler.request_redraw_at(100 * kMs));
    }
    CHECK_FALSE(scheduler.request_redraw_at(1 * kMs));
    CHECK(scheduler.ns_until_due(0) == 100 * kMs);
}

TEST_CASE("the budget follows the refresh rate")
{
    FrameScheduler scheduler{kFallbackMs};
    CHECK(scheduler.budget_ms() == kFallbackMs);

    scheduler.set_refresh_hz(60.0F);
    CHECK(scheduler.budget_ms() == Catch::Approx(16.667).margin(0.001));
    scheduler.set_refresh_hz(120.0F);
    CHECK(scheduler.budget_ms() == Catch::Approx(8.333).margin(0.001));

    scheduler.set_refresh_hz(0.0F);
    CHECK(scheduler.budget_ms() == kFallbackMs);
}

TEST_CASE("a frame misses when it finishes over 1.5 budgets after it was due")
{
    FrameScheduler scheduler{10.0};

    scheduler.request_redraw(0);
    scheduler.begin_frame(1 * kMs);
    CHECK_FALSE(scheduler.end_frame(15 * kMs));
    CHECK(scheduler.missed_frames() == 0);

    scheduler.request_redraw(20 * kMs);
    scheduler.begin_frame(21 * kMs);
    CHECK(scheduler.end_frame(36 * kMs));
    CHECK(scheduler.missed_frames() == 1);

    scheduler.reset_missed_frames();
    CHECK(scheduler.missed_frames() == 0);
}

// An idle gap before a request has to count for nothing, or every keystroke after a pause
// would register as a missed frame.
TEST_CASE("idling between frames isn't a miss")
{
    FrameScheduler scheduler{10.0};
    scheduler.request_redraw(0);
    scheduler.begin_frame(1 * kMs);
    scheduler.end_frame(2 * kMs);

    scheduler.request_redraw(5000 * kMs);
    scheduler.begin_frame(5001 * kMs);
    CHECK_FALSE(scheduler.end_frame(5002 * kMs));
}

TEST_CASE("a timed frame's lateness is measured from its deadline")
{
    FrameScheduler scheduler{10.0};
    REQUIRE(scheduler.request_redraw_at(100 * kMs));
    scheduler.begin_frame(120 * kMs);
    CHECK(scheduler.end_frame(121 * kMs));
}

TEST_CASE("end_frame without a due frame changes nothing")
{
    FrameScheduler scheduler{10.0};
    CHECK_FALSE(scheduler.end_frame(1000 * kMs));
    CHECK(scheduler.missed_frames() == 0);
}
