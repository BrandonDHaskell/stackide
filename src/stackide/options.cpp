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

} // namespace

void print_usage() {
    std::println(stderr,
                 "usage: stackide [--frames N] [--offscreen] [--headless] "
                 "[--timeout-ms N] [--allow-skip] [--gpu-debug]");
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
        } else if (arg == "--frames" || arg == "--timeout-ms") {
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
            } else {
                out.timeout_ms = static_cast<Uint64>(value);
            }
        } else {
            std::println(stderr, "unknown argument: {}", arg);
            return false;
        }
    }
    return true;
}

} // namespace stackide
