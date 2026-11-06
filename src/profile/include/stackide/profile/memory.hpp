#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <thread>

namespace stackide
{

    // Resident memory now, or 0 if it can't be read.
    std::uint64_t current_rss_bytes() noexcept;

    // The most resident memory this process has ever used, or 0 if it can't be read. From the
    // OS, so it catches spikes between samples.
    std::uint64_t peak_rss_bytes() noexcept;

    // The editor-only target in docs/hardware-environment.md.
    inline constexpr std::uint64_t kEditorMemoryLimitBytes = std::uint64_t{1} << 30U;

    // Samples memory in the background, plots it, and warns once if it passes the editor-only
    // target. On its own thread so that sampling can't keep an idle main loop awake.
    class MemorySampler
    {
    public:
        // on_sample is for tests, and runs on the sampling thread.
        explicit MemorySampler(std::chrono::milliseconds interval = std::chrono::seconds{1},
                               std::function<void(std::uint64_t)> on_sample = {});

    private:
        std::jthread thread_;
    };

} // namespace stackide
