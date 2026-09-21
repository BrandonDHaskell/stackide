#pragma once

#include <SDL3/SDL.h>

#include <stackide/constants.hpp>

#include <optional>

namespace stackide {

enum class Mode { Windowed, Offscreen, Headless };
enum class PresentMode { Vsync, Immediate };

struct Options {
    Mode mode = Mode::Windowed;
    PresentMode present_mode = PresentMode::Vsync;
    int frames = 0;                       // 0 means run until quit
    Uint64 timeout_ms = kDefaultTimeoutMs; // 0 disables the deadline
    bool allow_skip = false;
    bool gpu_debug = false;

    // M1 item 6: requires windowed mode with --present-mode immediate.
    std::optional<double> assert_p99_under_ms;
    // M1 item 7: requires windowed mode with --present-mode vsync.
    std::optional<int> assert_max_dropped_frames;
};

void print_usage();
bool parse_args(int argc, char** argv, Options& out);

} // namespace stackide
