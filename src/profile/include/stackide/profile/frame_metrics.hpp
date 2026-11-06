#pragma once

#include <stackide/profile/sample_window.hpp>

#include <array>
#include <cstddef>

namespace stackide
{

    struct MetricSummary
    {
        std::size_t count = 0;
        double p50_ms = 0.0;
        double p95_ms = 0.0;
        double p99_ms = 0.0;
        double max_ms = 0.0;
    };

    MetricSummary summarize(const SampleWindow& samples);

    // How many frames the on-screen graph shows.
    inline constexpr std::size_t kFrameHistorySize = 120;

    // The always-on numbers: percentiles over a long window, and the last few frames for the
    // graph. Fixed memory, and nothing here allocates per frame.
    class FrameMetrics
    {
    public:
        explicit FrameMetrics(std::size_t window_capacity) :
            frame_ms_(window_capacity), input_to_paint_ms_(window_capacity)
        {
        }

        // input_to_paint_ms is negative for a frame that reflected no input.
        void add_frame(double frame_ms, double input_to_paint_ms);

        [[nodiscard]] MetricSummary frame_summary() const { return summarize(frame_ms_); }
        [[nodiscard]] MetricSummary input_to_paint_summary() const { return summarize(input_to_paint_ms_); }

        // Oldest first, each relative to its limit so that 1.0 means exactly at the limit.
        // Slots with no value are negative.
        void normalized_history(double frame_limit_ms, double input_limit_ms,
                                std::array<float, kFrameHistorySize>& frames,
                                std::array<float, kFrameHistorySize>& inputs) const;

    private:
        SampleWindow frame_ms_;
        SampleWindow input_to_paint_ms_;
        std::array<float, kFrameHistorySize> recent_frame_ms_{};
        std::array<float, kFrameHistorySize> recent_input_ms_{};
        std::size_t next_ = 0;
        std::size_t recent_count_ = 0;
    };

} // namespace stackide
