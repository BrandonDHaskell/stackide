#include <stackide/base/options.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace
{

    // Empty on a rejected command line, so a test can't read a half-parsed result.
    std::optional<stackide::Options> parse(std::vector<std::string> args)
    {
        args.insert(args.begin(), "stackide");
        std::vector<char*> argv;
        argv.reserve(args.size());
        for (auto& arg : args)
        {
            argv.push_back(arg.data());
        }

        stackide::Options opts;
        if (!stackide::parse_args(static_cast<int>(argv.size()), argv.data(), opts))
        {
            return std::nullopt;
        }
        return opts;
    }

} // namespace

TEST_CASE("no arguments selects a windowed vsync run with no deadline")
{
    const auto opts = parse({});

    REQUIRE(opts);
    CHECK(opts->mode == stackide::Mode::Windowed);
    CHECK(opts->present_mode == stackide::PresentMode::Vsync);
    CHECK(opts->frames == 0);
    CHECK(opts->timeout_ms == 0);
    CHECK_FALSE(opts->allow_skip);
}

TEST_CASE("values are stored in their own fields")
{
    const auto opts = parse({"--frames", "12", "--timeout-ms", "34", "--resize-after-frames", "1", "--resize-width",
                             "640", "--resize-height", "480"});

    REQUIRE(opts);
    CHECK(opts->frames == 12);
    CHECK(opts->timeout_ms == std::uint64_t{34});
    CHECK(opts->resize_after_frames == 1);
    CHECK(opts->resize_width == 640);
    CHECK(opts->resize_height == 480);
}

TEST_CASE("zero is allowed where it means no limit")
{
    const auto opts = parse({"--frames", "0", "--timeout-ms", "0"});

    REQUIRE(opts);
    CHECK(opts->frames == 0);
    CHECK(opts->timeout_ms == std::uint64_t{0});
}

TEST_CASE("malformed values are rejected")
{
    CHECK_FALSE(parse({"--frames"}));
    CHECK_FALSE(parse({"--frames", "-1"}));
    CHECK_FALSE(parse({"--frames", "12x"}));
    CHECK_FALSE(parse({"--frames", ""}));
    CHECK_FALSE(parse({"--frames", "99999999999"}));
    CHECK_FALSE(parse({"--timeout-ms", "-1"}));
    CHECK_FALSE(parse({"--present-mode", "bogus"}));
    CHECK_FALSE(parse({"--assert-p99-under-ms", "0"}));
    CHECK_FALSE(parse({"--assert-p99-under-ms", "fast"}));
    CHECK_FALSE(parse({"--nonsense"}));
}

TEST_CASE("gates require the matching mode")
{
    CHECK(parse({"--present-mode", "immediate", "--assert-p99-under-ms", "2.0"}));
    CHECK(parse({"--assert-max-dropped-frames", "0"}));

    CHECK_FALSE(parse({"--assert-p99-under-ms", "2.0"}));
    CHECK_FALSE(parse({"--present-mode", "immediate", "--assert-max-dropped-frames", "0"}));
    CHECK_FALSE(parse({"--offscreen", "--assert-p99-under-ms", "2.0"}));
}

TEST_CASE("resize flags are all or nothing and windowed only")
{
    CHECK_FALSE(parse({"--resize-after-frames", "10"}));
    CHECK_FALSE(parse({"--resize-width", "640", "--resize-height", "480"}));
    CHECK_FALSE(parse({"--resize-after-frames", "0", "--resize-width", "640", "--resize-height", "480"}));
    CHECK_FALSE(
        parse({"--offscreen", "--resize-after-frames", "1", "--resize-width", "640", "--resize-height", "480"}));
}

TEST_CASE("the video driver is windowed only")
{
    const auto opts = parse({"--video-driver", "x11"});
    REQUIRE(opts);
    CHECK(opts->video_driver == stackide::VideoDriver::X11);

    CHECK_FALSE(parse({"--video-driver"}));
    CHECK_FALSE(parse({"--video-driver", "mir"}));
    CHECK_FALSE(parse({"--offscreen", "--video-driver", "x11"}));
}

TEST_CASE("requiring validation needs a debug device")
{
    CHECK(parse({"--gpu-debug", "--require-validation"}));
    CHECK_FALSE(parse({"--require-validation"}));
}

TEST_CASE("mailbox is a present mode")
{
    const auto opts = parse({"--present-mode", "mailbox"});
    REQUIRE(opts);
    CHECK(opts->present_mode == stackide::PresentMode::Mailbox);
}

TEST_CASE("injecting a stall is windowed only")
{
    const auto opts = parse({"--inject-stall-ms", "50"});
    REQUIRE(opts);
    CHECK(opts->inject_stall_ms == 50);

    CHECK_FALSE(parse({"--inject-stall-ms", "0"}));
    CHECK_FALSE(parse({"--offscreen", "--inject-stall-ms", "50"}));
}

TEST_CASE("a bounded run's default deadline grows with its length")
{
    const auto short_run = parse({"--frames", "10"});
    const auto long_run = parse({"--frames", "600"});
    REQUIRE(short_run);
    REQUIRE(long_run);
    CHECK(short_run->timeout_ms >= stackide::kDefaultTimeoutMs);
    // 600 frames at 60 Hz take 10 seconds, which has to fit with room to spare.
    CHECK(long_run->timeout_ms > 15'000);

    const auto explicit_deadline = parse({"--frames", "600", "--timeout-ms", "5"});
    REQUIRE(explicit_deadline);
    CHECK(explicit_deadline->timeout_ms == 5);
}

TEST_CASE("frame scheduler test flags are checked")
{
    CHECK(parse({"--idle-ms", "2000", "--redraw-every-ms", "100"}));
    CHECK(parse({"--inject-input", "5", "--assert-input-to-paint-under-ms", "8"}));
    CHECK(parse({"--frames", "5", "--assert-within-budget-percent", "99"}));
    CHECK(parse({"--frames", "5", "--inject-input", "5", "--input-sampling", "naive"}));

    CHECK_FALSE(parse({"--idle-ms", "10"}));
    CHECK_FALSE(parse({"--idle-ms", "2000", "--frames", "5"}));
    CHECK_FALSE(parse({"--idle-ms", "2000", "--inject-input", "5"}));
    CHECK_FALSE(parse({"--redraw-every-ms", "100"}));
    CHECK_FALSE(parse({"--assert-input-to-paint-under-ms", "8"}));
    CHECK_FALSE(parse({"--assert-within-budget-percent", "99"}));
    CHECK(parse({"--inject-input", "5", "--inject-interval-ms", "50"}));
    CHECK_FALSE(parse({"--inject-interval-ms", "50"}));
    CHECK_FALSE(parse({"--input-sampling", "sideways"}));
    CHECK_FALSE(parse({"--offscreen", "--inject-input", "5"}));
}
