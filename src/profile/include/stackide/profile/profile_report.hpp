#pragma once

#include <stackide/base/options.hpp>
#include <stackide/profile/frame_metrics.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace stackide
{

    struct ProfileReport
    {
        std::string_view mode;
        int frames_presented = 0;
        double frame_budget_ms = 0.0;
        std::uint64_t missed_frames = 0;
        MetricSummary frame;
        MetricSummary input_to_paint;
        std::uint64_t peak_rss_bytes = 0;
        std::uint64_t dropped_log_records = 0;
    };

    // Stable field names, because this is what later catches regressions in CI. mode must not
    // need JSON escaping: it comes from a fixed set of names, not from outside.
    std::string to_json(const ProfileReport& report);

    // Returns false, having logged why, if the file can't be written.
    bool write_profile_report(const ProfileReport& report, const std::filesystem::path& path);

    // Ends a --profile-frames run: fills in what the process knows about itself, writes the
    // report, and applies the memory gate. Returns false if either fails.
    bool finish_profile_run(const Options& options, ProfileReport report);

} // namespace stackide
