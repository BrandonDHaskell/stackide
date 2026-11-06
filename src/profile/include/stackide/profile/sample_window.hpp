#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace stackide
{

    // The most recent samples, in memory fixed at construction. Adding never allocates, so
    // this can run every frame for as long as the program does.
    class SampleWindow
    {
    public:
        // capacity must not be zero.
        explicit SampleWindow(std::size_t capacity) : samples_(capacity) {}

        void add(double sample) noexcept
        {
            samples_[next_] = sample; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            next_ = (next_ + 1) % samples_.size();
            count_ = std::min(count_ + 1, samples_.size());
        }

        [[nodiscard]] std::size_t size() const noexcept { return count_; }

        // Allocates, so it's for reporting rather than for the frame loop.
        [[nodiscard]] std::vector<double> sorted() const
        {
            std::vector<double> copy(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(count_));
            std::ranges::sort(copy);
            return copy;
        }

    private:
        std::vector<double> samples_;
        std::size_t next_ = 0;
        std::size_t count_ = 0;
    };

} // namespace stackide
