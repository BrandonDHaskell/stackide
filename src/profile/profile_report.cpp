#include <stackide/profile/profile_report.hpp>

#include <stackide/base/log.hpp>
#include <stackide/profile/memory.hpp>

#include <cstdint>
#include <format>
#include <fstream>
#include <print>

namespace stackide
{
    namespace
    {

        constexpr int kSchemaVersion = 1;

        std::string metric_json(const MetricSummary& metric)
        {
            return std::format(R"({{"count": {}, "p50": {:.3f}, "p95": {:.3f}, "p99": {:.3f}, "max": {:.3f}}})",
                               metric.count, metric.p50_ms, metric.p95_ms, metric.p99_ms, metric.max_ms);
        }

    } // namespace

    std::string to_json(const ProfileReport& report)
    {
        return std::format(R"({{
  "schema_version": {},
  "mode": "{}",
  "frames_presented": {},
  "frame_budget_ms": {:.3f},
  "missed_frames": {},
  "frame_ms": {},
  "input_to_paint_ms": {},
  "peak_rss_bytes": {},
  "dropped_log_records": {}
}}
)",
                           kSchemaVersion, report.mode, report.frames_presented, report.frame_budget_ms,
                           report.missed_frames, metric_json(report.frame), metric_json(report.input_to_paint),
                           report.peak_rss_bytes, report.dropped_log_records);
    }

    bool write_profile_report(const ProfileReport& report, const std::filesystem::path& path)
    {
        std::ofstream file{path};
        file << to_json(report);
        file.close();
        if (!file)
        {
            STACKIDE_LOG_ERROR(LogCategory::Profile, "writing the profile report to {} failed", path.string());
            return false;
        }
        return true;
    }

    bool finish_profile_run(const Options& options, ProfileReport report)
    {
        report.peak_rss_bytes = peak_rss_bytes();
        report.dropped_log_records = log_dropped_total();

        log_flush();
        constexpr std::uint64_t kBytesPerMegabyte = std::uint64_t{1} << 20U;
        std::println(stderr, "profile: wrote {}, frame p99 {:.3f} ms, peak memory {} MB", options.profile_output,
                     report.frame.p99_ms, report.peak_rss_bytes / kBytesPerMegabyte);
        if (!write_profile_report(report, options.profile_output))
        {
            return false;
        }

        if (options.assert_peak_rss_under_mb)
        {
            const auto limit = static_cast<std::uint64_t>(*options.assert_peak_rss_under_mb) * kBytesPerMegabyte;
            if (report.peak_rss_bytes > limit)
            {
                STACKIDE_LOG_ERROR(LogCategory::Profile, "peak memory {} bytes exceeds gate {} bytes",
                                   report.peak_rss_bytes, limit);
                return false;
            }
        }
        return true;
    }

} // namespace stackide
