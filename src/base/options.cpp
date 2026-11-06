#include <stackide/base/options.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <print>
#include <string_view>

namespace stackide
{
    namespace
    {

        // Below this, an idle check would mostly measure the window appearing.
        constexpr int kMinIdleMs = 1000;

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
            if (out.inject_stall_ms > 0 && out.mode != Mode::Windowed)
            {
                std::println(stderr, "--inject-stall-ms requires windowed mode");
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

        bool parse_input_sampling(int argc, char** argv, int& i, Options& out)
        {
            if (i + 1 >= argc)
            {
                std::println(stderr, "missing value for --input-sampling");
                return false;
            }
            const std::string_view value{argv[++i]};
            if (value == "late")
            {
                out.input_sampling = InputSampling::Late;
            }
            else if (value == "naive")
            {
                out.input_sampling = InputSampling::Naive;
            }
            else
            {
                std::println(stderr, "invalid value for --input-sampling: {}", value);
                return false;
            }
            return true;
        }

        bool is_scheduler_flag(std::string_view arg)
        {
            return arg == "--idle-ms" || arg == "--redraw-every-ms" || arg == "--inject-input" ||
                arg == "--inject-interval-ms" || arg == "--input-sampling" ||
                arg == "--assert-input-to-paint-under-ms" || arg == "--assert-within-budget-percent";
        }

        bool parse_scheduler_flag(int argc, char** argv, int& i, std::string_view arg, Options& out)
        {
            const auto positive_int = [](int v) { return v > 0; };
            const auto positive_double = [](double v) { return v > 0.0; };

            if (arg == "--idle-ms")
            {
                return next_int_arg(argc, argv, i, arg, positive_int, out.idle_ms);
            }
            if (arg == "--redraw-every-ms")
            {
                return next_int_arg(argc, argv, i, arg, positive_int, out.redraw_every_ms);
            }
            if (arg == "--inject-input")
            {
                return next_int_arg(argc, argv, i, arg, positive_int, out.inject_input);
            }
            if (arg == "--inject-interval-ms")
            {
                return next_int_arg(argc, argv, i, arg, positive_int, out.inject_interval_ms);
            }
            if (arg == "--input-sampling")
            {
                return parse_input_sampling(argc, argv, i, out);
            }

            double value = 0.0;
            const bool ok = next_double_arg(argc, argv, i, arg, positive_double, value);
            if (arg == "--assert-input-to-paint-under-ms")
            {
                out.assert_input_to_paint_under_ms = value;
            }
            else
            {
                out.assert_within_budget_percent = value;
            }
            return ok;
        }

        bool is_profile_flag(std::string_view arg)
        {
            return arg == "--profile-frames" || arg == "--profile-output" || arg == "--assert-peak-rss-under-mb" ||
                arg == "--show-frame-graph" || arg == "--assert-frame-graph-under-ms";
        }

        bool parse_profile_flag(int argc, char** argv, int& i, std::string_view arg, Options& out)
        {
            const auto positive_int = [](int v) { return v > 0; };
            if (arg == "--show-frame-graph")
            {
                out.show_frame_graph = true;
                return true;
            }
            if (arg == "--profile-frames")
            {
                return next_int_arg(argc, argv, i, arg, positive_int, out.profile_frames);
            }
            if (arg == "--assert-peak-rss-under-mb")
            {
                int value = 0;
                const bool ok = next_int_arg(argc, argv, i, arg, positive_int, value);
                out.assert_peak_rss_under_mb = value;
                return ok;
            }
            if (arg == "--assert-frame-graph-under-ms")
            {
                double value = 0.0;
                const bool ok = next_double_arg(argc, argv, i, arg, [](double v) { return v > 0.0; }, value);
                out.assert_frame_graph_under_ms = value;
                return ok;
            }
            if (i + 1 >= argc)
            {
                std::println(stderr, "missing value for {}", arg);
                return false;
            }
            out.profile_output = argv[++i];
            return true;
        }

        bool validate_profile_flags(Options& out)
        {
            if (out.profile_frames > 0 && out.mode == Mode::Headless)
            {
                std::println(stderr, "--profile-frames requires rendering, so it can't be headless");
                return false;
            }
            if (out.profile_frames > 0 && (out.frames > 0 || out.idle_ms > 0 || out.inject_input > 0))
            {
                std::println(stderr,
                             "--profile-frames sets its own frame count, so it can't be combined with "
                             "--frames, --idle-ms or --inject-input");
                return false;
            }
            if (out.assert_peak_rss_under_mb && out.profile_frames == 0)
            {
                std::println(stderr, "--assert-peak-rss-under-mb requires --profile-frames");
                return false;
            }
            if (out.show_frame_graph && out.mode != Mode::Windowed)
            {
                std::println(stderr, "--show-frame-graph requires windowed mode");
                return false;
            }
            if (out.assert_frame_graph_under_ms && !out.show_frame_graph)
            {
                std::println(stderr, "--assert-frame-graph-under-ms requires --show-frame-graph");
                return false;
            }
            if (out.profile_frames > 0 && out.mode == Mode::Windowed)
            {
                out.frames = out.profile_frames;
            }
            return true;
        }

        bool validate_scheduler_flags(const Options& out)
        {
            const bool any = out.idle_ms > 0 || out.redraw_every_ms > 0 || out.inject_input > 0 ||
                out.input_sampling != InputSampling::Late || out.assert_input_to_paint_under_ms ||
                out.assert_within_budget_percent;
            if (any && out.mode != Mode::Windowed)
            {
                std::println(stderr, "the frame scheduler test flags require windowed mode");
                return false;
            }
            if (out.idle_ms > 0 && (out.frames > 0 || out.inject_input > 0))
            {
                std::println(stderr, "--idle-ms can't be combined with --frames or --inject-input");
                return false;
            }
            if (out.idle_ms > 0 && out.idle_ms < kMinIdleMs)
            {
                std::println(stderr, "--idle-ms must be at least {}", kMinIdleMs);
                return false;
            }
            if (out.redraw_every_ms > 0 && out.idle_ms == 0)
            {
                std::println(stderr, "--redraw-every-ms requires --idle-ms");
                return false;
            }
            if (out.inject_interval_ms > 0 && out.inject_input == 0)
            {
                std::println(stderr, "--inject-interval-ms requires --inject-input");
                return false;
            }
            if (out.assert_input_to_paint_under_ms && out.inject_input == 0)
            {
                std::println(stderr, "--assert-input-to-paint-under-ms requires --inject-input");
                return false;
            }
            if (out.assert_within_budget_percent && out.frames == 0)
            {
                std::println(stderr, "--assert-within-budget-percent requires --frames");
                return false;
            }
            return true;
        }

        // Only a run with an end needs a deadline: an editor waiting for its user doesn't.
        void apply_default_timeout(Options& out)
        {
            if (out.frames > 0)
            {
                out.timeout_ms = kDefaultTimeoutMs + (static_cast<std::uint64_t>(out.frames) * kTimeoutMsPerFrame);
            }
            else if (out.inject_input > 0)
            {
                // Twice the interval, because injected events are spread around it.
                const auto per_event =
                    std::max<std::uint64_t>(kTimeoutMsPerFrame, static_cast<std::uint64_t>(out.inject_interval_ms) * 2);
                out.timeout_ms = kDefaultTimeoutMs + (static_cast<std::uint64_t>(out.inject_input) * per_event);
            }
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
                     "[--idle-ms N [--redraw-every-ms N]] [--inject-input N [--inject-interval-ms N]] "
                     "[--input-sampling late|naive] "
                     "[--assert-input-to-paint-under-ms MS] [--assert-within-budget-percent P] "
                     "[--profile-frames N [--profile-output PATH] [--assert-peak-rss-under-mb N]] "
                     "[--show-frame-graph [--assert-frame-graph-under-ms MS]] [--inject-stall-ms N] [--allow-skip] "
                     "[--gpu-debug [--require-validation]]");
    }

    bool parse_args(int argc, char** argv, Options& out)
    {
        const auto non_negative = [](int v) { return v >= 0; };
        const auto positive_int = [](int v) { return v > 0; };
        const auto positive_double = [](double v) { return v > 0.0; };
        bool timeout_given = false;

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
                timeout_given = true;
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
            else if (arg == "--inject-stall-ms")
            {
                ok = next_int_arg(argc, argv, i, arg, positive_int, out.inject_stall_ms);
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
            else if (is_profile_flag(arg))
            {
                ok = parse_profile_flag(argc, argv, i, arg, out);
            }
            else if (is_scheduler_flag(arg))
            {
                ok = parse_scheduler_flag(argc, argv, i, arg, out);
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

        if (!validate_profile_flags(out))
        {
            return false;
        }
        if (!timeout_given)
        {
            apply_default_timeout(out);
        }
        return validate_resize_flags(out) && validate_gate_flags(out) && validate_gpu_flags(out) &&
            validate_scheduler_flags(out);
    }

} // namespace stackide
