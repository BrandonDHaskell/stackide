#include <stackide/platform/frame_scheduler.hpp>

#include <algorithm>

namespace stackide::platform
{
    namespace
    {

        constexpr double kMillisPerSecond = 1000.0;
        constexpr double kNanosPerMilli = 1'000'000.0;
        // M1 doc's fixed measurement definition of a dropped frame.
        constexpr double kMissedFrameMultiplier = 1.5;

    } // namespace

    void FrameScheduler::set_refresh_hz(float hz) noexcept
    {
        budget_ms_ = hz > 0.0F ? kMillisPerSecond / static_cast<double>(hz) : fallback_budget_ms_;
    }

    void FrameScheduler::request_redraw(std::uint64_t now_ns) noexcept
    {
        if (!requested_since_ns_)
        {
            requested_since_ns_ = now_ns;
        }
    }

    bool FrameScheduler::request_redraw_at(std::uint64_t when_ns)
    {
        if (timed_count_ == timed_.size())
        {
            return false;
        }
        timed_.at(timed_count_) = when_ns;
        ++timed_count_;
        return true;
    }

    std::optional<std::uint64_t> FrameScheduler::ns_until_due(std::uint64_t now_ns) const noexcept
    {
        if (requested_since_ns_)
        {
            return 0;
        }
        if (timed_count_ == 0)
        {
            return std::nullopt;
        }
        const std::uint64_t earliest = *std::min_element(timed_.begin(), timed_.begin() + timed_count_);
        return earliest > now_ns ? earliest - now_ns : 0;
    }

    bool FrameScheduler::frame_due(std::uint64_t now_ns) const noexcept
    {
        const auto until_due = ns_until_due(now_ns);
        return until_due && *until_due == 0;
    }

    void FrameScheduler::begin_frame(std::uint64_t now_ns)
    {
        frame_due_since_ns_ = requested_since_ns_;
        requested_since_ns_.reset();

        std::size_t kept = 0;
        for (std::size_t i = 0; i < timed_count_; ++i)
        {
            const std::uint64_t when = timed_.at(i);
            if (when > now_ns)
            {
                timed_.at(kept) = when;
                ++kept;
            }
            else if (!frame_due_since_ns_ || when < *frame_due_since_ns_)
            {
                frame_due_since_ns_ = when;
            }
        }
        timed_count_ = kept;
    }

    bool FrameScheduler::end_frame(std::uint64_t now_ns) noexcept
    {
        if (!frame_due_since_ns_)
        {
            return false;
        }
        const double late_ms = static_cast<double>(now_ns - *frame_due_since_ns_) / kNanosPerMilli;
        frame_due_since_ns_.reset();
        if (late_ms > kMissedFrameMultiplier * budget_ms_)
        {
            ++missed_frames_;
            return true;
        }
        return false;
    }

} // namespace stackide::platform
