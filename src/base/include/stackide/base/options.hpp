#pragma once

#include <stackide/base/constants.hpp>

#include <cstdint>
#include <optional>

namespace stackide
{

    enum class Mode : std::uint8_t
    {
        Windowed,
        Offscreen,
        Headless,
    };
    enum class PresentMode : std::uint8_t
    {
        Vsync,
        Mailbox,
        Immediate,
    };
    enum class VideoDriver : std::uint8_t
    {
        Native,
        Wayland,
        X11,
    };

    struct Options
    {
        Mode mode = Mode::Windowed;
        PresentMode present_mode = PresentMode::Vsync;
        int frames = 0; // 0 means run until quit
        std::uint64_t timeout_ms = kDefaultTimeoutMs; // 0 disables the deadline
        bool allow_skip = false;
        bool gpu_debug = false;
        bool require_validation = false; // requires gpu_debug
        VideoDriver video_driver = VideoDriver::Native; // requires windowed mode

        // M1 item 6: requires windowed mode with --present-mode immediate.
        std::optional<double> assert_p99_under_ms;
        // M1 item 7: requires windowed mode with --present-mode vsync.
        std::optional<int> assert_max_dropped_frames;

        // Makes one main-loop iteration this slow, to prove stalls get caught. Requires
        // windowed mode.
        int inject_stall_ms = 0;

        // M1 item 8: requires windowed mode. All three or none.
        int resize_after_frames = 0; // 0 means no programmatic resize
        int resize_width = 0;
        int resize_height = 0;
    };

    void print_usage();
    bool parse_args(int argc, char** argv, Options& out);

} // namespace stackide
