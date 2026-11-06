#include <stackide/profile/frame_metrics.hpp>
#include <stackide/profile/memory.hpp>
#include <stackide/profile/profile.hpp>
#include <stackide/profile/profile_report.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>

using namespace stackide;
using Catch::Matchers::ContainsSubstring;

#if !STACKIDE_PROFILING
TEST_CASE("with profiling off, the macros don't evaluate their arguments")
{
    int evaluations = 0;
    const auto argument = [&] { return ++evaluations; };
    STACKIDE_ZONE("name");
    STACKIDE_FRAME();
    STACKIDE_PLOT("name", argument());
    STACKIDE_THREAD_NAME("name");
    static_cast<void>(argument);
    CHECK(evaluations == 0);
}
#endif

TEST_CASE("a summary reports percentiles of the whole window")
{
    FrameMetrics metrics{1000};
    for (int i = 1; i <= 100; ++i)
    {
        metrics.add_frame(static_cast<double>(i), -1.0);
    }
    const auto frames = metrics.frame_summary();
    CHECK(frames.count == 100);
    CHECK(frames.p50_ms == 50.0);
    CHECK(frames.p95_ms == 95.0);
    CHECK(frames.p99_ms == 99.0);
    CHECK(frames.max_ms == 100.0);
}

TEST_CASE("only frames that reflected input count toward input to paint")
{
    FrameMetrics metrics{10};
    metrics.add_frame(1.0, -1.0);
    metrics.add_frame(1.0, 5.0);
    metrics.add_frame(1.0, -1.0);
    metrics.add_frame(1.0, 0.0);
    const auto inputs = metrics.input_to_paint_summary();
    CHECK(inputs.count == 2);
    CHECK(inputs.max_ms == 5.0);
}

TEST_CASE("an empty summary is all zeros")
{
    const FrameMetrics metrics{4};
    CHECK(metrics.input_to_paint_summary().count == 0);
    CHECK(metrics.input_to_paint_summary().p99_ms == 0.0);
}

TEST_CASE("the graph history is oldest first with the newest at the right edge")
{
    FrameMetrics metrics{4};
    metrics.add_frame(5.0, -1.0);
    metrics.add_frame(10.0, 4.0);

    std::array<float, kFrameHistorySize> frames{};
    std::array<float, kFrameHistorySize> inputs{};
    metrics.normalized_history(10.0, 8.0, frames, inputs);

    CHECK(frames.at(0) < 0.0F);
    CHECK(frames.at(kFrameHistorySize - 3) < 0.0F);
    CHECK(frames.at(kFrameHistorySize - 2) == 0.5F);
    CHECK(frames.at(kFrameHistorySize - 1) == 1.0F);
    CHECK(inputs.at(kFrameHistorySize - 2) < 0.0F);
    CHECK(inputs.at(kFrameHistorySize - 1) == 0.5F);
}

TEST_CASE("the graph history keeps only the most recent frames")
{
    FrameMetrics metrics{4};
    for (std::size_t i = 0; i < kFrameHistorySize + 5; ++i)
    {
        metrics.add_frame(static_cast<double>(i), -1.0);
    }
    std::array<float, kFrameHistorySize> frames{};
    std::array<float, kFrameHistorySize> inputs{};
    metrics.normalized_history(1.0, 1.0, frames, inputs);
    CHECK(frames.at(0) == 5.0F);
    CHECK(frames.at(kFrameHistorySize - 1) == static_cast<float>(kFrameHistorySize + 4));
}

TEST_CASE("memory readings are plausible and follow real use")
{
    const auto before = current_rss_bytes();
    CHECK(before > 0);
    CHECK(peak_rss_bytes() >= before);

    constexpr std::size_t kSize = std::size_t{64} << 20U;
    const auto block = std::make_unique<char[]>(kSize);
    // Volatile, because an optimizer may remove writes to memory that's never read.
    volatile char* bytes = block.get();
    constexpr std::size_t kPage = 4096;
    for (std::size_t i = 0; i < kSize; i += kPage)
    {
        bytes[i] = 1;
    }
    const auto during = current_rss_bytes();
    CHECK(during >= before + (kSize / 2));
    CHECK(peak_rss_bytes() >= during);
}

TEST_CASE("the memory sampler samples repeatedly and stops promptly")
{
    using namespace std::chrono_literals;
    std::atomic<int> samples{0};
    std::atomic<std::uint64_t> last_rss{0};
    const auto start = std::chrono::steady_clock::now();
    {
        const MemorySampler sampler{1ms, [&](std::uint64_t rss)
                                    {
                                        last_rss.store(rss);
                                        samples.fetch_add(1);
                                    }};
        while (samples.load() < 3)
        {
            std::this_thread::yield();
        }
    }
    {
        // A long interval has to stop at once rather than wait it out.
        const MemorySampler sleepy{1h};
    }
    CHECK(last_rss.load() > 0);
    CHECK(std::chrono::steady_clock::now() - start < 10s);
}

TEST_CASE("the report has every field")
{
    ProfileReport report;
    report.mode = "offscreen";
    report.frames_presented = 3;
    report.frame_budget_ms = 16.0;
    report.frame = MetricSummary{.count = 3, .p50_ms = 1.0, .p95_ms = 2.0, .p99_ms = 3.0, .max_ms = 4.0};
    report.peak_rss_bytes = 123;

    const auto json = to_json(report);
    CHECK_THAT(json, ContainsSubstring(R"("schema_version": 1)"));
    CHECK_THAT(json, ContainsSubstring(R"("mode": "offscreen")"));
    CHECK_THAT(json, ContainsSubstring(R"("frames_presented": 3)"));
    CHECK_THAT(json, ContainsSubstring(R"("p99": 3.000)"));
    CHECK_THAT(json, ContainsSubstring(R"("input_to_paint_ms": {"count": 0)"));
    CHECK_THAT(json, ContainsSubstring(R"("peak_rss_bytes": 123)"));
}

TEST_CASE("an unwritable report path is reported")
{
    const ProfileReport report;
    CHECK_FALSE(write_profile_report(report, std::filesystem::path{"/nonexistent-directory/report.json"}));
}
