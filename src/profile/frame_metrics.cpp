#include <stackide/profile/frame_metrics.hpp>

#include <stackide/profile/percentile.hpp>

#include <vector>

namespace stackide
{
    namespace
    {

        constexpr double kP50 = 0.50;
        constexpr double kP95 = 0.95;
        constexpr double kP99 = 0.99;

    } // namespace

    MetricSummary summarize(const SampleWindow& samples)
    {
        const std::vector<double> sorted = samples.sorted();
        if (sorted.empty())
        {
            return {};
        }
        return MetricSummary{
            .count = sorted.size(),
            .p50_ms = percentile(sorted, kP50),
            .p95_ms = percentile(sorted, kP95),
            .p99_ms = percentile(sorted, kP99),
            .max_ms = sorted.back(),
        };
    }

    void FrameMetrics::add_frame(double frame_ms, double input_to_paint_ms)
    {
        frame_ms_.add(frame_ms);
        if (input_to_paint_ms >= 0.0)
        {
            input_to_paint_ms_.add(input_to_paint_ms);
        }

        recent_frame_ms_.at(next_) = static_cast<float>(frame_ms);
        recent_input_ms_.at(next_) = static_cast<float>(input_to_paint_ms);
        next_ = (next_ + 1) % kFrameHistorySize;
        if (recent_count_ < kFrameHistorySize)
        {
            ++recent_count_;
        }
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): named by the header, called once
    void FrameMetrics::normalized_history(double frame_limit_ms, double input_limit_ms,
                                          std::array<float, kFrameHistorySize>& frames,
                                          std::array<float, kFrameHistorySize>& inputs) const
    {
        const auto frame_scale = static_cast<float>(1.0 / frame_limit_ms);
        const auto input_scale = static_cast<float>(1.0 / input_limit_ms);
        const std::size_t oldest = (next_ + kFrameHistorySize - recent_count_) % kFrameHistorySize;
        const std::size_t empty = kFrameHistorySize - recent_count_;

        frames.fill(-1.0F);
        inputs.fill(-1.0F);
        for (std::size_t i = 0; i < recent_count_; ++i)
        {
            const std::size_t source = (oldest + i) % kFrameHistorySize;
            // Empty slots go first, so that the newest frame is always at the right edge.
            frames.at(empty + i) = recent_frame_ms_.at(source) * frame_scale;
            const float input = recent_input_ms_.at(source);
            inputs.at(empty + i) = input >= 0.0F ? input * input_scale : -1.0F;
        }
    }

} // namespace stackide
