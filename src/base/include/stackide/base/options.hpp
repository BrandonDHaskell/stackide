#pragma once

#include <stackide/base/constants.hpp>

#include <cstdint>
#include <optional>
#include <string>

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
    enum class InputSampling : std::uint8_t
    {
        Late, // read input after waiting for the display, just before recording a frame
        Naive, // read input only before that wait, to measure what late sampling gains
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
        int frames = 0; // 0 means draw on demand until quit
        std::uint64_t timeout_ms = 0; // 0 disables the deadline. Defaults by what's bounded
        bool allow_skip = false;
        bool gpu_debug = false;
        bool require_validation = false; // requires gpu_debug
        VideoDriver video_driver = VideoDriver::Native; // requires windowed mode

        // M1 item 6: requires windowed mode with --present-mode immediate.
        std::optional<double> assert_p99_under_ms;
        // M1 item 7: requires windowed mode with --present-mode vsync.
        std::optional<int> assert_max_dropped_frames;

        InputSampling input_sampling = InputSampling::Late; // requires windowed mode

        // Each requires windowed mode, and exists to test the frame scheduler. idle_ms runs
        // for that long, then fails unless the frame count and CPU use match an idle editor.
        // redraw_every_ms adds a timed redraw to that, as a blinking cursor would.
        // inject_input queues that many synthetic input events.
        int idle_ms = 0;
        int redraw_every_ms = 0;
        int inject_input = 0;
        int inject_interval_ms = 0; // about this far apart; 0 means faster than a refresh
        std::optional<double> assert_input_to_paint_under_ms; // p99, requires inject_input
        std::optional<double> assert_within_budget_percent; // requires frames

        // Renders this many frames, windowed or offscreen, then writes a JSON summary.
        int profile_frames = 0;
        std::string profile_output = "stackide-profile.json";
        std::optional<int> assert_peak_rss_under_mb; // requires profile_frames

        bool show_frame_graph = false; // requires windowed mode
        std::optional<double> assert_frame_graph_under_ms; // p99, requires show_frame_graph

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
