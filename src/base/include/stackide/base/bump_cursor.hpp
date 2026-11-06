#pragma once

#include <cstdint>
#include <optional>

namespace stackide
{

    // Hands out offsets from a fixed-size region, from the start again after each reset.
    class BumpCursor
    {
    public:
        explicit constexpr BumpCursor(std::uint32_t capacity) noexcept : capacity_(capacity) {}

        // Empty if the request doesn't fit, leaving the cursor unchanged. alignment must
        // be a power of two.
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
        constexpr std::optional<std::uint32_t> reserve(std::uint32_t size, std::uint32_t alignment = 1) noexcept
        {
            // 64-bit so that a request near the limit can't wrap around and appear to fit.
            const std::uint64_t mask = std::uint64_t{alignment} - 1;
            const std::uint64_t start = (std::uint64_t{used_} + mask) & ~mask;
            if (start + size > capacity_)
            {
                return std::nullopt;
            }
            used_ = static_cast<std::uint32_t>(start + size);
            return static_cast<std::uint32_t>(start);
        }

        constexpr void reset() noexcept { used_ = 0; }

        [[nodiscard]] constexpr std::uint32_t used() const noexcept { return used_; }

    private:
        std::uint32_t capacity_;
        std::uint32_t used_ = 0;
    };

} // namespace stackide
