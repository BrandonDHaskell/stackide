#include <stackide/base/options.hpp>

#include <charconv>
#include <cstdint>
#include <print>
#include <string_view>

namespace stackide
{
    namespace
    {

        bool parse_int(std::string_view text, int& out)
        {
            const auto* first = text.data();
            const auto* last = first + text.size();
            const auto result = std::from_chars(first, last, out);
            return result.ec == std::errc{} && result.ptr == last;
        }

        bool parse_double(std::string_view text, double& out)
        {
            const auto* first = text.data();
            const auto* last = first + text.size();
            const auto result = std::from_chars(first, last, out);
            return result.ec == std::errc{} && result.ptr == last;
        }

        // Consumes argv[++i] as the value for `flag`, validated by `pred`. Prints
        // the same "missing value"/"invalid value" messages every flag uses.
        template <class Pred>
        bool next_int_arg(int argc, char** argv, int& i, std::string_view flag, Pred pred, int& out)
        {
            if (i + 1 >= argc)
            {
                std::println(stderr, "missing value for {}", flag);
                return false;
            }
            int value = 0;
            if (!parse_int(argv[++i], value) || !pred(value))
            {
                std::println(stderr, "invalid value for {}: {}", flag, argv[i]);
                return false;
            }
            out = value;
            return true;
        }

        template <class Pred>
        bool next_double_arg(int argc, char** argv, int& i, std::string_view flag, Pred pred, double& out)
        {
            if (i + 1 >= argc)
            {
                std::println(stderr, "missing value for {}", flag);
                return false;
            }
            double value = 0.0;
            if (!parse_double(argv[++i], value) || !pred(value))
            {
                std::println(stderr, "invalid value for {}: {}", flag, argv[i]);
                return false;
            }
            out = value;
            return true;
        }

        bool parse_present_mode(int argc, char** argv, int& i, Options& out)
        {
            if (i + 1 >= argc)
            {
                std::println(stderr, "missing value for --present-mode");
                return false;
            }
            const std::string_view value{argv[++i]};
            if (value == "vsync")
            {
                out.present_mode = PresentMode::Vsync;
            }
            else if (value == "mailbox")
            {
                out.present_mode = PresentMode::Mailbox;
            }
            else if (value == "immediate")
            {
                out.present_mode = PresentMode::Immediate;
            }
            else
            {
                std::println(stderr, "invalid value for --present-mode: {}", value);
                return false;
            }
            return true;
        }

        bool parse_video_driver(int argc, char** argv, int& i, Options& out)
        {
            if (i + 1 >= argc)
            {
                std::println(stderr, "missing value for --video-driver");
                return false;
            }
            const std::string_view value{argv[++i]};
            if (value == "wayland")
            {
                out.video_driver = VideoDriver::Wayland;
            }
            else if (value == "x11")
            {
                out.video_driver = VideoDriver::X11;
            }
            else
            {
                std::println(stderr, "invalid value for --video-driver: {}", value);
                return false;
            }
            return true;
        }

        bool validate_gpu_flags(const Options& out)
        {
            if (out.video_driver != VideoDriver::Native && out.mode != Mode::Windowed)
            {
                std::println(stderr, "--video-driver requires windowed mode");
                return false;
            }
            if (out.require_validation && !out.gpu_debug)
            {
                std::println(stderr, "--require-validation requires --gpu-debug");
                return false;
            }
            return true;
        }

        bool validate_resize_flags(const Options& out)
        {
            const bool any = out.resize_after_frames > 0 || out.resize_width > 0 || out.resize_height > 0;
            const bool all = out.resize_after_frames > 0 && out.resize_width > 0 && out.resize_height > 0;
            if (any && !all)
            {
                std::println(stderr,
                             "--resize-after-frames, --resize-width, and --resize-height "
                             "must all be given together");
                return false;
            }
            if (all && out.mode != Mode::Windowed)
            {
                std::println(stderr, "--resize-after-frames requires windowed mode");
                return false;
            }
            return true;
        }

        bool validate_gate_flags(const Options& out)
        {
            if (out.assert_p99_under_ms && (out.mode != Mode::Windowed || out.present_mode != PresentMode::Immediate))
            {
                std::println(stderr, "--assert-p99-under-ms requires windowed mode with --present-mode immediate");
                return false;
            }
            if (out.assert_max_dropped_frames && (out.mode != Mode::Windowed || out.present_mode != PresentMode::Vsync))
            {
                std::println(stderr, "--assert-max-dropped-frames requires windowed mode with --present-mode vsync");
                return false;
            }
            return true;
        }

    } // namespace

    void print_usage()
    {
        std::println(stderr,
                     "usage: stackide [--frames N] [--offscreen] [--headless] "
                     "[--timeout-ms N] [--present-mode vsync|mailbox|immediate] [--video-driver wayland|x11] "
                     "[--assert-p99-under-ms MS] [--assert-max-dropped-frames N] "
                     "[--resize-after-frames N --resize-width W --resize-height H] "
                     "[--allow-skip] [--gpu-debug [--require-validation]]");
    }

    bool parse_args(int argc, char** argv, Options& out)
    {
        const auto non_negative = [](int v) { return v >= 0; };
        const auto positive_int = [](int v) { return v > 0; };
        const auto positive_double = [](double v) { return v > 0.0; };

        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg{argv[i]};
            bool ok = true;

            if (arg == "--offscreen")
            {
                out.mode = Mode::Offscreen;
            }
            else if (arg == "--headless")
            {
                out.mode = Mode::Headless;
            }
            else if (arg == "--allow-skip")
            {
                out.allow_skip = true;
            }
            else if (arg == "--gpu-debug")
            {
                out.gpu_debug = true;
            }
            else if (arg == "--require-validation")
            {
                out.require_validation = true;
            }
            else if (arg == "--video-driver")
            {
                ok = parse_video_driver(argc, argv, i, out);
            }
            else if (arg == "--present-mode")
            {
                ok = parse_present_mode(argc, argv, i, out);
            }
            else if (arg == "--frames")
            {
                ok = next_int_arg(argc, argv, i, arg, non_negative, out.frames);
            }
            else if (arg == "--timeout-ms")
            {
                int value = 0;
                ok = next_int_arg(argc, argv, i, arg, non_negative, value);
                out.timeout_ms = static_cast<std::uint64_t>(value);
            }
            else if (arg == "--assert-max-dropped-frames")
            {
                int value = 0;
                ok = next_int_arg(argc, argv, i, arg, non_negative, value);
                out.assert_max_dropped_frames = value;
            }
            else if (arg == "--assert-p99-under-ms")
            {
                double value = 0.0;
                ok = next_double_arg(argc, argv, i, arg, positive_double, value);
                out.assert_p99_under_ms = value;
            }
            else if (arg == "--resize-after-frames")
            {
                ok = next_int_arg(argc, argv, i, arg, positive_int, out.resize_after_frames);
            }
            else if (arg == "--resize-width")
            {
                ok = next_int_arg(argc, argv, i, arg, positive_int, out.resize_width);
            }
            else if (arg == "--resize-height")
            {
                ok = next_int_arg(argc, argv, i, arg, positive_int, out.resize_height);
            }
            else
            {
                std::println(stderr, "unknown argument: {}", arg);
                ok = false;
            }

            if (!ok)
            {
                return false;
            }
        }

        return validate_resize_flags(out) && validate_gate_flags(out) && validate_gpu_flags(out);
    }

} // namespace stackide
