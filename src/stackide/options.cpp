#include <stackide/options.hpp>

#include <charconv>
#include <print>
#include <string_view>

namespace stackide {
namespace {

bool parse_int(std::string_view text, int& out) {
    const auto* first = text.data();
    const auto* last = first + text.size();
    const auto result = std::from_chars(first, last, out);
    return result.ec == std::errc{} && result.ptr == last;
}

bool parse_double(std::string_view text, double& out) {
    const auto* first = text.data();
    const auto* last = first + text.size();
    const auto result = std::from_chars(first, last, out);
    return result.ec == std::errc{} && result.ptr == last;
}

} // namespace

void print_usage() {
    std::println(stderr,
                 "usage: stackide [--frames N] [--offscreen] [--headless] "
                 "[--timeout-ms N] [--present-mode vsync|immediate] "
                 "[--assert-p99-under-ms MS] [--assert-max-dropped-frames N] "
                 "[--allow-skip] [--gpu-debug]");
}

bool parse_args(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};

        if (arg == "--offscreen") {
            out.mode = Mode::Offscreen;
        } else if (arg == "--headless") {
            out.mode = Mode::Headless;
        } else if (arg == "--allow-skip") {
            out.allow_skip = true;
        } else if (arg == "--gpu-debug") {
            out.gpu_debug = true;
        } else if (arg == "--present-mode") {
            if (i + 1 >= argc) {
                std::println(stderr, "missing value for {}", arg);
                return false;
            }
            const std::string_view value{argv[++i]};
            if (value == "vsync") {
                out.present_mode = PresentMode::Vsync;
            } else if (value == "immediate") {
                out.present_mode = PresentMode::Immediate;
            } else {
                std::println(stderr, "invalid value for --present-mode: {}", value);
                return false;
            }
        } else if (arg == "--frames" || arg == "--timeout-ms" ||
                   arg == "--assert-max-dropped-frames") {
            if (i + 1 >= argc) {
                std::println(stderr, "missing value for {}", arg);
                return false;
            }
            int value = 0;
            if (!parse_int(argv[++i], value) || value < 0) {
                std::println(stderr, "invalid value for {}: {}", arg, argv[i]);
                return false;
            }
            if (arg == "--frames") {
                out.frames = value;
            } else if (arg == "--timeout-ms") {
                out.timeout_ms = static_cast<Uint64>(value);
            } else {
                out.assert_max_dropped_frames = value;
            }
        } else if (arg == "--assert-p99-under-ms") {
            if (i + 1 >= argc) {
                std::println(stderr, "missing value for {}", arg);
                return false;
            }
            double value = 0.0;
            if (!parse_double(argv[++i], value) || value <= 0.0) {
                std::println(stderr, "invalid value for {}: {}", arg, argv[i]);
                return false;
            }
            out.assert_p99_under_ms = value;
        } else {
            std::println(stderr, "unknown argument: {}", arg);
            return false;
        }
    }

    if (out.assert_p99_under_ms &&
        (out.mode != Mode::Windowed || out.present_mode != PresentMode::Immediate)) {
        std::println(stderr,
                     "--assert-p99-under-ms requires windowed mode with --present-mode immediate");
        return false;
    }
    if (out.assert_max_dropped_frames &&
        (out.mode != Mode::Windowed || out.present_mode != PresentMode::Vsync)) {
        std::println(stderr,
                     "--assert-max-dropped-frames requires windowed mode with --present-mode vsync");
        return false;
    }

    return true;
}

} // namespace stackide
