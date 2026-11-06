#include <stackide/render/modes.hpp>

#include <stackide/base/constants.hpp>
#include <stackide/base/log.hpp>
#include <stackide/core/core.hpp>
#include <stackide/platform/event.hpp>
#include <stackide/platform/main_loop.hpp>
#include <stackide/platform/video.hpp>
#include <stackide/platform/window.hpp>
#include <stackide/profile/percentile.hpp>
#include <stackide/profile/stall_detector.hpp>
#include <stackide/render/gpu.hpp>
#include <stackide/render/quad_resources.hpp>
#include <stackide/render/render_constants.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <print>
#include <thread>
#include <type_traits>
#include <variant>
#include <vector>

namespace stackide
{
    namespace
    {

        using Clock = std::chrono::steady_clock;

        // M1 doc's fixed measurement definition: a frame interval over 1.5x the
        // expected refresh interval counts as dropped. Not a CTest-tunable knob.
        constexpr double kDropIntervalMultiplier = 1.5;

        // Only used when --frames is unbounded; a bounded run reserves exactly that
        // many slots instead.
        constexpr std::size_t kDefaultFrameCapacityReserve = 4096;
        constexpr Uint32 kOccludedFrameDelayMs = 16;

        // The working budget from docs/hardware-environment.md, for when the display can't
        // say what its refresh rate is.
        constexpr double kFallbackFrameBudgetMs = 16.0;

        // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members): a bundle of
        // arguments, which never outlives the function that declares what it refers to.
        struct Renderer
        {
            const render::Device& device;
            const render::Swapchain& swapchain;
            platform::Window& window;
            const QuadResources& quad;
        };
        // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)

        // Shrinks whichever NDC axis is longer so the (square) checkerboard quad
        // doesn't stretch to fill a non-square window.
        std::array<float, 2> letterbox_scale(platform::PixelSize size)
        {
            if (size.width <= 0 || size.height <= 0)
            {
                return {1.0F, 1.0F};
            }
            const double aspect = static_cast<double>(size.width) / static_cast<double>(size.height);
            if (aspect >= 1.0)
            {
                return {static_cast<float>(1.0 / aspect), 1.0F};
            }
            return {1.0F, static_cast<float>(aspect)};
        }

        void handle_event(const platform::Event& event, const platform::Window& window, bool& quit)
        {
            switch (event.type)
            {
            case platform::EventType::Quit:
                quit = true;
                break;
            case platform::EventType::WindowResized:
                STACKIDE_LOG_DEBUG(LogCategory::Platform, "window resized to {}x{} pixels", event.size.width,
                                   event.size.height);
                break;
            case platform::EventType::ScaleChanged:
                STACKIDE_LOG_INFO(LogCategory::Platform, "display scale changed to {:.2f}",
                                  static_cast<double>(window.display_scale()));
                break;
            case platform::EventType::DisplayChanged:
                STACKIDE_LOG_INFO(LogCategory::Platform, "display changed, refresh rate {:.2f} Hz",
                                  static_cast<double>(window.refresh_hz()));
                break;
            case platform::EventType::Wakeup:
                // Only here to end a wait: pump() drains the messages next anyway.
                break;
            }
        }

        void handle_message(const CoreToUi& message)
        {
            std::visit(
                [](const auto& value)
                {
                    using Message = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Message, ProgressBegin>)
                    {
                        STACKIDE_LOG_INFO(LogCategory::Core, "progress {}: started {}", value.token.value, value.title);
                    }
                    else if constexpr (std::is_same_v<Message, ProgressEnd>)
                    {
                        STACKIDE_LOG_INFO(LogCategory::Core, "progress {}: ended {}", value.token.value, value.message);
                    }
                    // Reports and redraws have nowhere to go until there's a UI to show
                    // them and a scheduler to request frames from (#8).
                },
                message);
        }

        // Handles everything pending, returning whether a quit was among it.
        bool pump_main_loop(platform::MainLoop& loop, const platform::Window& window)
        {
            bool quit = false;
            loop.pump([&](const platform::Event& event) { handle_event(event, window, quit); },
                      [](const CoreToUi& message) { handle_message(message); });
            return quit;
        }

        double elapsed_ms(Clock::time_point from, Clock::time_point to)
        {
            return std::chrono::duration<double, std::milli>(to - from).count();
        }

        struct RenderedFrame
        {
            // False if the window was minimized/occluded: not an error, just nothing
            // to present.
            bool presented = false;
            // Under vsync, this is where the wait for the display happens.
            double swapchain_wait_ms = 0.0;
        };

        // Renders and submits one frame. Returns nullopt on a real failure.
        std::optional<RenderedFrame> render_frame(const Renderer& renderer, StallDetector& stalls)
        {
            stalls.phase("acquiring a command buffer");
            std::optional<render::CommandBuffer> commands = render::CommandBuffer::acquire(renderer.device);
            if (!commands)
            {
                return std::nullopt;
            }

            // Waiting for the display isn't work, and neither is the occluded delay below.
            stalls.pause();
            const auto wait_start = Clock::now();
            const std::optional<SDL_GPUTexture*> target = commands->wait_for_swapchain_texture(renderer.swapchain);
            if (!target)
            {
                return std::nullopt;
            }
            const double swapchain_wait_ms = elapsed_ms(wait_start, Clock::now());

            if (*target == nullptr)
            {
                // Minimized or occluded: not an error, but no frame was presented.
                commands->submit();
                SDL_Delay(kOccludedFrameDelayMs);
                return RenderedFrame{.presented = false, .swapchain_wait_ms = swapchain_wait_ms};
            }

            stalls.phase("recording and submitting the frame");
            {
                std::optional<render::RenderPass> pass = commands->begin_render_pass(*target, clear_color());
                if (!pass)
                {
                    return std::nullopt;
                }
                draw_quad(*commands, *pass, renderer.quad, letterbox_scale(renderer.window.size_in_pixels()));
            }
            commands->submit();
            return RenderedFrame{.presented = true, .swapchain_wait_ms = swapchain_wait_ms};
        }

        enum class WarmupResult : std::uint8_t
        {
            Completed,
            QuitRequested,
            Failed,
        };

        // Absorbs the one-time swapchain/compositor negotiation cost that the first
        // several presented frames pay on a freshly-mapped window (observed: one
        // frame stalling ~2.8x the refresh interval, consistently within the first
        // ~10 frames, regardless of how long the process waits beforehand -- it is
        // tied to frame count, not wall-clock time). Polling drains the event queue,
        // so a quit seen here has to be reported or it is lost.
        WarmupResult run_warmup_frames(const Renderer& renderer, platform::MainLoop& loop, int count)
        {
            // Never ended: these frames are known to be slow, so they aren't reported.
            StallDetector unreported{kFallbackFrameBudgetMs};
            for (int i = 0; i < count; ++i)
            {
                if (pump_main_loop(loop, renderer.window))
                {
                    return WarmupResult::QuitRequested;
                }
                if (!render_frame(renderer, unreported))
                {
                    return WarmupResult::Failed;
                }
            }
            return WarmupResult::Completed;
        }

        struct FrameStats
        {
            int exit_code = EXIT_SUCCESS;
            int presented = 0;
            int dropped_frames = 0;
            std::vector<double> frame_times_ms;
        };

        // The measures are distinct on purpose: cost is what a frame took to build and
        // goes in the percentiles, interval is what the display saw and decides drops.
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
        void record_presented_frame(FrameStats& stats, double cost_ms, double interval_ms, double expected_interval_ms)
        {
            ++stats.presented;
            if (expected_interval_ms > 0.0 && stats.presented > 1 &&
                interval_ms > kDropIntervalMultiplier * expected_interval_ms)
            {
                ++stats.dropped_frames;
            }
            stats.frame_times_ms.push_back(cost_ms);
        }

        void inject_stall_once(const Options& opts, StallDetector& stalls, bool& injected)
        {
            if (opts.inject_stall_ms > 0 && !injected)
            {
                stalls.phase("injected stall");
                std::this_thread::sleep_for(std::chrono::milliseconds{opts.inject_stall_ms});
                injected = true;
            }
        }

        // Runs the measured loop: presents frames until quit, --frames is reached,
        // or --timeout-ms elapses, tracking per-frame timing and dropped-frame
        // counts along the way.
        FrameStats run_measured_loop(const Renderer& renderer, platform::MainLoop& loop, const Options& opts,
                                     double expected_interval_ms)
        {
            StallDetector stalls{expected_interval_ms > 0.0 ? expected_interval_ms : kFallbackFrameBudgetMs};
            bool stall_injected = false;

            FrameStats stats;
            stats.frame_times_ms.reserve(opts.frames > 0 ? static_cast<std::size_t>(opts.frames)
                                                         : kDefaultFrameCapacityReserve);

            auto last = Clock::now();
            const Uint64 started_ms = SDL_GetTicks();
            bool running = true;

            while (running)
            {
                const auto frame_start = Clock::now();
                stalls.begin_iteration();

                stalls.phase("handling events and messages");
                if (pump_main_loop(loop, renderer.window))
                {
                    break;
                }

                const std::optional<RenderedFrame> frame = render_frame(renderer, stalls);
                if (!frame)
                {
                    stats.exit_code = EXIT_FAILURE;
                    break;
                }

                inject_stall_once(opts, stalls, stall_injected);
                stalls.end_iteration();

                if (frame->presented)
                {
                    const auto now = Clock::now();
                    // Only vsync's wait is excluded. In immediate mode the same wait is
                    // GPU back-pressure, a real cost the frame-cost gate has to see.
                    const double excluded_ms = opts.present_mode == PresentMode::Vsync ? frame->swapchain_wait_ms : 0.0;
                    record_presented_frame(stats, elapsed_ms(frame_start, now) - excluded_ms, elapsed_ms(last, now),
                                           expected_interval_ms);
                    last = now;

                    if (opts.resize_after_frames > 0 && stats.presented == opts.resize_after_frames)
                    {
                        renderer.window.set_size(opts.resize_width, opts.resize_height);
                    }

                    if (opts.frames > 0 && stats.presented >= opts.frames)
                    {
                        running = false;
                    }
                }

                if (opts.timeout_ms > 0 && SDL_GetTicks() - started_ms > opts.timeout_ms)
                {
                    STACKIDE_LOG_ERROR(LogCategory::Render, "timeout after {} ms with {} frame(s) presented",
                                       opts.timeout_ms, stats.presented);
                    stats.exit_code = EXIT_FAILURE;
                    break;
                }
            }

            return stats;
        }

        // Prints the exit-time summary and applies items 6/7's gates. Returns the
        // (possibly updated) exit code.
        int report_and_gate(const Options& opts, const FrameStats& stats, double expected_interval_ms)
        {
            int exit_code = stats.exit_code;

            if (exit_code == EXIT_SUCCESS && opts.frames > 0 && stats.presented < opts.frames)
            {
                STACKIDE_LOG_ERROR(LogCategory::Render, "requested {} frame(s), presented {}", opts.frames,
                                   stats.presented);
                exit_code = EXIT_FAILURE;
            }

            std::vector<double> sorted = stats.frame_times_ms;
            std::ranges::sort(sorted);
            const double p50 = percentile(sorted, 0.50);
            const double p99 = percentile(sorted, 0.99);
            const double max_ms = sorted.empty() ? 0.0 : sorted.back();

            // The summary is program output, not a log line, so queued logs go first.

            log_flush();

            if (expected_interval_ms > 0.0)
            {
                std::println(stderr,
                             "frames presented: {}, cpu frame time p50={:.3f} ms p99={:.3f} ms "
                             "max={:.3f} ms, dropped={}",
                             stats.presented, p50, p99, max_ms, stats.dropped_frames);
            }
            else
            {
                std::println(stderr,
                             "frames presented: {}, cpu frame time p50={:.3f} ms p99={:.3f} ms "
                             "max={:.3f} ms, dropped=n/a (refresh rate unknown)",
                             stats.presented, p50, p99, max_ms);
            }

            if (exit_code == EXIT_SUCCESS && opts.assert_p99_under_ms && p99 > *opts.assert_p99_under_ms)
            {
                STACKIDE_LOG_ERROR(LogCategory::Render, "p99 frame time {:.3f} ms exceeds gate {:.3f} ms", p99,
                                   *opts.assert_p99_under_ms);
                exit_code = EXIT_FAILURE;
            }
            if (exit_code == EXIT_SUCCESS && opts.assert_max_dropped_frames &&
                stats.dropped_frames > *opts.assert_max_dropped_frames)
            {
                STACKIDE_LOG_ERROR(LogCategory::Render, "dropped {} frame(s), gate allows {}", stats.dropped_frames,
                                   *opts.assert_max_dropped_frames);
                exit_code = EXIT_FAILURE;
            }

            return exit_code;
        }

        platform::VideoBackend to_video_backend(VideoDriver driver)
        {
            switch (driver)
            {
            case VideoDriver::Native:
                return platform::VideoBackend::Native;
            case VideoDriver::Wayland:
                return platform::VideoBackend::Wayland;
            case VideoDriver::X11:
                return platform::VideoBackend::X11;
            }
            return platform::VideoBackend::Native;
        }

        int render_window(const Options& opts)
        {
            std::optional<platform::Window> window = platform::Window::create("stackide", kWindowWidth, kWindowHeight);
            if (!window)
            {
                return EXIT_FAILURE;
            }

            const std::optional<render::Device> device = render::Device::create(opts.gpu_debug);
            if (!device)
            {
                return render::gpu_unavailable_code(opts);
            }

            const std::optional<render::Swapchain> swapchain =
                render::Swapchain::create(*device, *window, opts.present_mode);
            if (!swapchain)
            {
                return render::gpu_unavailable_code(opts);
            }

            STACKIDE_LOG_INFO(LogCategory::Render, "swapchain format: {}", static_cast<int>(swapchain->format()));
            const auto [width, height] = window->size_in_pixels();
            STACKIDE_LOG_INFO(LogCategory::Platform, "window: {}x{} pixels, display scale {:.2f}", width, height,
                              static_cast<double>(window->display_scale()));

            const float refresh_hz = window->refresh_hz();
            STACKIDE_LOG_INFO(LogCategory::Platform, "display refresh rate: {:.2f} Hz",
                              static_cast<double>(refresh_hz));
            const double expected_interval_ms = refresh_hz > 0.0F ? 1000.0 / static_cast<double>(refresh_hz) : 0.0;

            const std::optional<QuadResources> quad = create_quad_resources(*device, swapchain->format());
            if (!quad)
            {
                return render::gpu_unavailable_code(opts);
            }

            // After everything the core could need, and before the loop that drains it.
            Core core{[] { platform::post_wakeup(); }};
            platform::MainLoop loop{core.to_ui()};
            const Renderer renderer{.device = *device, .swapchain = *swapchain, .window = *window, .quad = *quad};

            constexpr int kWarmupFrames = 20;
            const WarmupResult warmup = run_warmup_frames(renderer, loop, kWarmupFrames);
            if (warmup == WarmupResult::Failed)
            {
                return EXIT_FAILURE;
            }

            const FrameStats stats = warmup == WarmupResult::QuitRequested
                ? FrameStats{}
                : run_measured_loop(renderer, loop, opts, expected_interval_ms);
            return report_and_gate(opts, stats, expected_interval_ms);
        }

    } // namespace

    int run_windowed(const Options& opts)
    {
        const std::optional<platform::Video> video = platform::Video::start(to_video_backend(opts.video_driver));
        if (!video)
        {
            return render::gpu_unavailable_code(opts);
        }
        STACKIDE_LOG_INFO(LogCategory::Platform, "video driver: {}", video->driver_name());
        return render::apply_gpu_debug_result(opts, render_window(opts));
    }

} // namespace stackide
