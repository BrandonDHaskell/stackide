//
// Created by bhaskell on 9/14/26.
//
// stackide: application entry point and smoke-test harness.
//
// Modes:
//   (default)     windowed render loop, runs until quit
//   --frames N    windowed, exits after N presented frames (N=0 means unbounded)
//   --offscreen   no window; clears an owned texture, reads it back, asserts pixels
//   --headless    no GPU device at all; init/teardown lifecycle only
//
// Exit codes:
//   0   success
//   1   failure
//   77  skipped (no GPU backend available, only with --allow-skip)

#include <SDL3/SDL.h>

#include <stackide/modes.hpp>
#include <stackide/options.hpp>
#include <stackide/scope_exit.hpp>

#include <cstdlib>
#include <print>

namespace stackide {
namespace {

// ---------------------------------------------------------------------------
// Headless: lifecycle only, no GPU device, no window.
// ---------------------------------------------------------------------------

int run_headless() {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::println(stderr, "SDL_Init failed: {}", SDL_GetError());
        return EXIT_FAILURE;
    }
    ScopeExit quit{[] { SDL_Quit(); }};

    std::println(stderr, "headless: video driver = {}", SDL_GetCurrentVideoDriver());
    return EXIT_SUCCESS;
}

} // namespace
} // namespace stackide

int main(int argc, char** argv) {
    stackide::Options opts;
    if (!stackide::parse_args(argc, argv, opts)) {
        stackide::print_usage();
        return EXIT_FAILURE;
    }

    switch (opts.mode) {
        case stackide::Mode::Headless:  return stackide::run_headless();
        case stackide::Mode::Offscreen: return stackide::run_offscreen(opts);
        case stackide::Mode::Windowed:  return stackide::run_windowed(opts);
    }

    return EXIT_FAILURE;
}
