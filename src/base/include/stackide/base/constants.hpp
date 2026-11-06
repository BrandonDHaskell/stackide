#pragma once

#include <cstddef>
#include <cstdint>

#if defined(__x86_64__) && !defined(__AVX2__)
#error "stackide's x86-64 baseline is AVX2 (docs/hardware-environment.md). Build through a preset."
#endif

namespace stackide
{

    // Per architecture, never detected at runtime (docs/hardware-environment.md).
#ifdef __x86_64__
    inline constexpr std::size_t kCacheLineBytes = 64;
#elif defined(__aarch64__) && defined(__APPLE__)
    inline constexpr std::size_t kCacheLineBytes = 128;
#else
#error "No cache line size is specified for this target in docs/hardware-environment.md."
#endif

    // CTest convention for "skipped" (see SKIP_RETURN_CODE).
    inline constexpr int kExitSkip = 77;

    // For a bounded run: the base, and an allowance per frame that a 20 Hz display could
    // meet, so that a run's own length can't make it time out on a slow display.
    inline constexpr std::uint64_t kDefaultTimeoutMs = 10'000;
    inline constexpr std::uint64_t kTimeoutMsPerFrame = 50;

} // namespace stackide
