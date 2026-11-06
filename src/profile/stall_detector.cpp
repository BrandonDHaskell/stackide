#include <stackide/profile/stall_detector.hpp>

#include <stackide/base/log.hpp>

namespace stackide
{

    void StallDetector::begin_iteration() noexcept
    {
        total_ms_ = 0.0;
        longest_ms_ = 0.0;
        longest_name_ = "";
        longest_location_ = {};
        current_name_ = nullptr;
    }

    void StallDetector::close_phase(Clock::time_point now) noexcept
    {
        if (current_name_ == nullptr)
        {
            return;
        }
        const double elapsed = std::chrono::duration<double, std::milli>(now - phase_start_).count();
        total_ms_ += elapsed;
        if (elapsed > longest_ms_)
        {
            longest_ms_ = elapsed;
            longest_name_ = current_name_;
            longest_location_ = current_location_;
        }
        current_name_ = nullptr;
    }

    void StallDetector::phase(const char* name, std::source_location location) noexcept
    {
        const auto now = Clock::now();
        close_phase(now);
        current_name_ = name;
        current_location_ = location;
        phase_start_ = now;
    }

    void StallDetector::pause() noexcept { close_phase(Clock::now()); }

    std::optional<Stall> StallDetector::end_iteration() noexcept
    {
        close_phase(Clock::now());
        if (total_ms_ <= budget_ms_)
        {
            return std::nullopt;
        }

        STACKIDE_LOG_WARN(
            LogCategory::Profile, "main loop stall: {:.1f} ms over a {:.1f} ms budget, mostly {} at {}:{} ({:.1f} ms)",
            total_ms_, budget_ms_, longest_name_, longest_location_.file_name(), longest_location_.line(), longest_ms_);
        return Stall{
            .total_ms = total_ms_,
            .budget_ms = budget_ms_,
            .longest_phase_ms = longest_ms_,
            .longest_phase = longest_name_,
            .file = longest_location_.file_name(),
            .line = longest_location_.line(),
        };
    }

} // namespace stackide
