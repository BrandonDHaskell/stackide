//
// Created by bhaskell on 9/14/26.
//
// stackide: application entry point and smoke-test harness.
//
// Modes:
//   (default)     windowed render loop, runs until quit
//   --frames N    windowed, exits after N presented frames (N=0 means unbounded)
//   --offscreen   no window; renders the quad to a texture, reads it back, asserts pixels
//   --headless    no GPU device at all; init/teardown lifecycle only
//
// Exit codes:
//   0   success
//   1   failure
//   77  skipped (no GPU backend available, only with --allow-skip)

#include <stackide/base/log.hpp>
#include <stackide/base/options.hpp>
#include <stackide/platform/video.hpp>
#include <stackide/render/modes.hpp>

#include <cstdlib>
#include <exception>
#include <optional>
#include <print>

namespace stackide
{
    namespace
    {

        // ---------------------------------------------------------------------------
        // Headless: lifecycle only, no GPU device, no window.
        // ---------------------------------------------------------------------------

        int run_headless()
        {
            const std::optional<platform::Video> video = platform::Video::start(platform::VideoBackend::Dummy);
            if (!video)
            {
                return EXIT_FAILURE;
            }
            STACKIDE_LOG_INFO(LogCategory::Platform, "headless: video driver = {}", video->driver_name());
            return EXIT_SUCCESS;
        }

    } // namespace
} // namespace stackide

// The handler's own println can still throw if writing to stderr fails. With
// stderr gone there is nowhere left to report to, so terminating is acceptable.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv)
{
    try
    {
        stackide::Options opts;
        if (!stackide::parse_args(argc, argv, opts))
        {
            stackide::print_usage();
            return EXIT_FAILURE;
        }

        // CI sets this so a broken GPU path fails instead of passing as a skip.
        if (const char* require_gpu = std::getenv("STACKIDE_REQUIRE_GPU");
            require_gpu != nullptr && *require_gpu != '\0')
        {
            opts.allow_skip = false;
        }

        const stackide::LogSession logging{stackide::log_config_from_environment()};

        switch (opts.mode)
        {
        case stackide::Mode::Headless:
            return stackide::run_headless();
        case stackide::Mode::Offscreen:
            return stackide::run_offscreen(opts);
        case stackide::Mode::Windowed:
            return stackide::run_windowed(opts);
        }

        return EXIT_FAILURE;
    }
    catch (const std::exception& e)
    {
        std::println(stderr, "unhandled exception: {}", e.what());
        return EXIT_FAILURE;
    }
}
