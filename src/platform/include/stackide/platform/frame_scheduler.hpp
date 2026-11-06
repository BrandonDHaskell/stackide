#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace stackide::platform
{

    // Decides when a frame should be drawn: nothing draws unless something asked for it, so
    // an idle editor keeps the machine idle. A blinking cursor is a timed request, not a
    // render loop. Main thread only: other threads ask through a RedrawRequested message.
    // Times are in nanoseconds, from any one clock.
    class FrameScheduler
    {
    public:
        static constexpr std::size_t kMaxTimedRequests = 32;

        // fallback_budget_ms applies until a usable refresh rate is known.
        explicit FrameScheduler(double fallback_budget_ms) noexcept :
            fallback_budget_ms_(fallback_budget_ms), budget_ms_(fallback_budget_ms)
        {
        }

        // Derives the budget, falling back if hz is 0, which means the rate is unknown.
        void set_refresh_hz(float hz) noexcept;
        [[nodiscard]] double budget_ms() const noexcept { return budget_ms_; }

        void request_redraw(std::uint64_t now_ns) noexcept;
        // False if too many are pending, in which case this one is not scheduled.
        [[nodiscard]] bool request_redraw_at(std::uint64_t when_ns);

        [[nodiscard]] bool frame_due(std::uint64_t now_ns) const noexcept;
        // How long a sleep can last. Empty if nothing is pending, and 0 if a frame is due.
        [[nodiscard]] std::optional<std::uint64_t> ns_until_due(std::uint64_t now_ns) const noexcept;
        [[nodiscard]] std::size_t timed_requests_pending() const noexcept { return timed_count_; }

        // Call just before recording, once input has been read: whatever is due now gets
        // satisfied by this frame. Requests made after this need another frame.
        void begin_frame(std::uint64_t now_ns);

        // Call when a frame was presented. Returns true if it missed: it took over 1.5
        // budgets from when it was first due, which is the M1 definition of a dropped
        // frame. Measured from due time, so that idling between frames isn't a miss.
        bool end_frame(std::uint64_t now_ns) noexcept;

        [[nodiscard]] std::uint64_t missed_frames() const noexcept { return missed_frames_; }
        void reset_missed_frames() noexcept { missed_frames_ = 0; }

    private:
        double fallback_budget_ms_;
        double budget_ms_;
        std::optional<std::uint64_t> requested_since_ns_;
        std::array<std::uint64_t, kMaxTimedRequests> timed_{};
        std::size_t timed_count_ = 0;
        std::optional<std::uint64_t> frame_due_since_ns_;
        std::uint64_t missed_frames_ = 0;
    };

} // namespace stackide::platform
