#pragma once

#include <SDL3/SDL.h>

#include <stackide/constants.hpp>

namespace stackide {

enum class Mode { Windowed, Offscreen, Headless };

struct Options {
    Mode mode = Mode::Windowed;
    int frames = 0;                       // 0 means run until quit
    Uint64 timeout_ms = kDefaultTimeoutMs; // 0 disables the deadline
    bool allow_skip = false;
    bool gpu_debug = false;
};

void print_usage();
bool parse_args(int argc, char** argv, Options& out);

} // namespace stackide
