#pragma once

#include <chrono>
#include <optional>
#include <source_location>

namespace stackide
{

    struct Stall
    {
        double total_ms = 0.0;
        double budget_ms = 0.0;
        double longest_phase_ms = 0.0;
        const char* longest_phase = "";
        const char* file = "";
        unsigned line = 0;
    };

    // Flags main-loop iterations over budget, saying which part took the longest. A long
    // wait isn't work, so it should be excluded with pause():
    //
    //     detector.begin_iteration();
    //     detector.phase("events");
    //     ...
    //     detector.pause();              // waiting for the display
    //     ...
    //     detector.phase("render");      // phase() resumes timing
    //     ...
    //     detector.end_iteration();
    //
    // Doesn't allocate. An iteration that never ends goes unreported.
    class StallDetector
    {
    public:
        explicit StallDetector(double budget_ms) noexcept : budget_ms_(budget_ms) {}

        void set_budget_ms(double budget_ms) noexcept { budget_ms_ = budget_ms; }

        void begin_iteration() noexcept;
        void phase(const char* name, std::source_location location = std::source_location::current()) noexcept;
        void pause() noexcept;
        // Logs a warning and returns the details if the iteration went over budget.
        std::optional<Stall> end_iteration() noexcept;

    private:
        using Clock = std::chrono::steady_clock;

        void close_phase(Clock::time_point now) noexcept;

        double budget_ms_;
        double total_ms_ = 0.0;
        double longest_ms_ = 0.0;
        const char* current_name_ = nullptr;
        std::source_location current_location_;
        const char* longest_name_ = "";
        std::source_location longest_location_;
        Clock::time_point phase_start_;
    };

} // namespace stackide
