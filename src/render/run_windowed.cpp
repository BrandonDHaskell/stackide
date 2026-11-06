#include <stackide/render/modes.hpp>

#include <stackide/base/constants.hpp>
#include <stackide/base/log.hpp>
#include <stackide/core/core.hpp>
#include <stackide/platform/event.hpp>
#include <stackide/platform/frame_scheduler.hpp>
#include <stackide/platform/main_loop.hpp>
#include <stackide/platform/video.hpp>
#include <stackide/platform/window.hpp>
#include <stackide/profile/percentile.hpp>
#include <stackide/profile/sample_window.hpp>
#include <stackide/profile/stall_detector.hpp>
#include <stackide/render/gpu.hpp>
#include <stackide/render/quad_resources.hpp>
#include <stackide/render/render_constants.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <print>
#include <stop_token>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace stackide
{
    namespace
    {

        constexpr double kNanosPerMilli = 1'000'000.0;
        constexpr std::uint64_t kNanosPerMilliInt = 1'000'000;
        constexpr double kMillisPerSecond = 1000.0;
        constexpr double kPercent = 100.0;
        constexpr double kP50 = 0.50;
        constexpr double kP99 = 0.99;

        // Fixed memory for a run of any length.
        constexpr std::size_t kDefaultSampleCapacity = 4096;
        constexpr Uint32 kOccludedFrameDelayMs = 16;

        // The working budget from docs/hardware-environment.md, for when the display can't
        // say what its refresh rate is.
        constexpr double kFallbackFrameBudgetMs = 16.0;

        // How long an idle check lets the window appear and the compositor settle.
        constexpr std::uint64_t kIdleSettleMs = 500;
        // The target in issue #8.
        constexpr double kIdleMaxCpuPercent = 1.0;
        // Timers and compositors aren't exact, so timed redraws get this much slack.
        constexpr double kTimedRedrawTolerance = 0.3;

        // Without a set interval, synthetic input arrives irregularly in this range, so that
        // it lands at every point in the refresh interval rather than beating against it.
        constexpr std::uint32_t kInjectMinMs = 3;
        constexpr std::uint32_t kInjectSpreadMs = 14;

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

        struct LoopState
        {
            // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
            LoopState(double budget_ms, std::size_t frame_capacity) :
                scheduler(budget_ms), stalls(budget_ms), frame_times_ms(frame_capacity),
                input_to_paint_ms(kDefaultSampleCapacity)
            {
            }

            platform::FrameScheduler scheduler;
            StallDetector stalls;
            bool quit = false;
            // The oldest input the next frame will reflect.
            std::optional<std::uint64_t> pending_input_ns;

            int presented = 0;
            std::uint64_t frames_within_budget = 0;
            int exit_code = EXIT_SUCCESS;
            SampleWindow frame_times_ms;
            SampleWindow input_to_paint_ms;

            // Whether the window system asked for the next frame, and how many it has asked for.
            bool frame_requested_by_event = false;
            int frames_from_input = 0;
            int frames_from_window = 0;
            bool next_frame_from_input = false;

            // For --idle-ms: what happened after the window settled.
            int idle_frames = 0;
            int idle_frames_from_input = 0;
            int idle_frames_from_window = 0;
            double idle_cpu_percent = 0.0;
        };

        double ns_to_ms(std::uint64_t ns) { return static_cast<double>(ns) / kNanosPerMilli; }

        // Shrinks whichever NDC axis is longer so the (square) checkerboard quad
        // doesn't stretch to fill a non-square window.
        std::array<float, 2> letterbox_scale(Uint32 width, Uint32 height)
        {
            if (width == 0 || height == 0)
            {
                return {1.0F, 1.0F};
            }
            const double aspect = static_cast<double>(width) / static_cast<double>(height);
            if (aspect >= 1.0)
            {
                return {static_cast<float>(1.0 / aspect), 1.0F};
            }
            return {1.0F, static_cast<float>(aspect)};
        }

        void apply_refresh_rate(LoopState& state, const platform::Window& window)
        {
            state.scheduler.set_refresh_hz(window.refresh_hz());
            state.stalls.set_budget_ms(state.scheduler.budget_ms());
        }

        void handle_event(const platform::Event& event, const platform::Window& window, LoopState& state)
        {
            const std::uint64_t now = platform::now_ns();
            switch (event.type)
            {
            case platform::EventType::Quit:
                state.quit = true;
                break;
            case platform::EventType::WindowResized:
                STACKIDE_LOG_DEBUG(LogCategory::Platform, "window resized to {}x{} pixels", event.size.width,
                                   event.size.height);
                state.frame_requested_by_event = true;
                state.scheduler.request_redraw(now);
                break;
            case platform::EventType::ScaleChanged:
                STACKIDE_LOG_INFO(LogCategory::Platform, "display scale changed to {:.2f}",
                                  static_cast<double>(window.display_scale()));
                state.frame_requested_by_event = true;
                state.scheduler.request_redraw(now);
                break;
            case platform::EventType::DisplayChanged:
                apply_refresh_rate(state, window);
                STACKIDE_LOG_INFO(LogCategory::Platform, "display changed, frame budget {:.2f} ms",
                                  state.scheduler.budget_ms());
                state.frame_requested_by_event = true;
                state.scheduler.request_redraw(now);
                break;
            case platform::EventType::Exposed:
                state.frame_requested_by_event = true;
                state.scheduler.request_redraw(now);
                break;
            case platform::EventType::Input:
                if (!state.pending_input_ns)
                {
                    state.pending_input_ns = event.timestamp_ns;
                }
                state.next_frame_from_input = true;
                state.scheduler.request_redraw(now);
                break;
            case platform::EventType::Wakeup:
                // Only here to end a wait: pump() drains the messages next anyway.
                break;
            }
        }

        void handle_message(const CoreToUi& message, LoopState& state)
        {
            std::visit(
                [&state](const auto& value)
                {
                    using Message = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Message, RedrawRequested>)
                    {
                        state.scheduler.request_redraw(platform::now_ns());
                    }
                    else if constexpr (std::is_same_v<Message, ProgressBegin>)
                    {
                        STACKIDE_LOG_INFO(LogCategory::Core, "progress {}: started {}", value.token.value, value.title);
                    }
                    else if constexpr (std::is_same_v<Message, ProgressEnd>)
                    {
                        STACKIDE_LOG_INFO(LogCategory::Core, "progress {}: ended {}", value.token.value, value.message);
                    }
                },
                message);
        }

        void pump_main_loop(platform::MainLoop& loop, const platform::Window& window, LoopState& state)
        {
            loop.pump([&](const platform::Event& event) { handle_event(event, window, state); },
                      [&](const CoreToUi& message) { handle_message(message, state); });
        }

        struct RenderedFrame
        {
            // False if the window was minimized/occluded: not an error, just nothing
            // to present.
            bool presented = false;
            // Under vsync, this is where the wait for the display happens.
            std::uint64_t swapchain_wait_ns = 0;
        };

        // Renders and submits one frame. Returns nullopt on a real failure.
        std::optional<RenderedFrame> render_frame(const Renderer& renderer, platform::MainLoop& loop, LoopState& state,
                                                  InputSampling sampling)
        {
            state.stalls.phase("acquiring a command buffer");
            std::optional<render::CommandBuffer> commands = render::CommandBuffer::acquire(renderer.device);
            if (!commands)
            {
                return std::nullopt;
            }

            // Waiting for the display isn't work, and neither is the occluded delay below.
            state.stalls.pause();
            const std::uint64_t wait_start = platform::now_ns();
            const std::optional<render::SwapchainTarget> target =
                commands->wait_for_swapchain_texture(renderer.swapchain);
            if (!target)
            {
                return std::nullopt;
            }
            const std::uint64_t swapchain_wait_ns = platform::now_ns() - wait_start;

            // The wait can be most of a frame, and input that arrived during it can still
            // make this frame. Reading it now is the whole point of late sampling.
            if (sampling == InputSampling::Late)
            {
                state.stalls.phase("handling late events and messages");
                pump_main_loop(loop, renderer.window, state);
                state.stalls.pause();
            }
            state.scheduler.begin_frame(platform::now_ns());

            if (target->texture == nullptr)
            {
                // Minimized or occluded: not an error, but no frame was presented. Whatever
                // was asked for gets drawn when the window next becomes visible, which
                // sends an event of its own.
                commands->submit();
                SDL_Delay(kOccludedFrameDelayMs);
                return RenderedFrame{.presented = false, .swapchain_wait_ns = swapchain_wait_ns};
            }

            state.stalls.phase("recording and submitting the frame");
            {
                std::optional<render::RenderPass> pass = commands->begin_render_pass(target->texture, clear_color());
                if (!pass)
                {
                    return std::nullopt;
                }
                draw_quad(*commands, *pass, renderer.quad, letterbox_scale(target->width, target->height));
            }
            commands->submit();

            // A resize read late doesn't change a texture that was already acquired, so the
            // new size needs one more frame.
            const auto [width, height] = renderer.window.size_in_pixels();
            if (std::cmp_not_equal(width, target->width) || std::cmp_not_equal(height, target->height))
            {
                state.scheduler.request_redraw(platform::now_ns());
            }
            return RenderedFrame{.presented = true, .swapchain_wait_ns = swapchain_wait_ns};
        }

        // Absorbs the one-time swapchain/compositor negotiation cost that the first
        // several presented frames pay on a freshly-mapped window (observed: one
        // frame stalling ~2.8x the refresh interval, consistently within the first
        // ~10 frames, regardless of how long the process waits beforehand -- it is
        // tied to frame count, not wall-clock time). Returns false on a render failure.
        bool run_warmup_frames(const Renderer& renderer, platform::MainLoop& loop, LoopState& state, int count)
        {
            for (int i = 0; i < count && !state.quit; ++i)
            {
                pump_main_loop(loop, renderer.window, state);
                state.scheduler.request_redraw(platform::now_ns());
                const auto frame = render_frame(renderer, loop, state, InputSampling::Late);
                if (!frame)
                {
                    return false;
                }
                if (frame->presented)
                {
                    state.scheduler.end_frame(platform::now_ns());
                }
            }
            state.scheduler.reset_missed_frames();
            return true;
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

        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
        void record_presented_frame(LoopState& state, const Options& opts, std::uint64_t frame_start_ns,
                                    std::uint64_t swapchain_wait_ns)
        {
            const std::uint64_t now = platform::now_ns();
            state.scheduler.end_frame(now);
            ++state.presented;

            // Only vsync's wait is excluded. In immediate mode the same wait is GPU
            // back-pressure, a real cost the frame-cost gate has to see.
            const std::uint64_t excluded_ns = opts.present_mode == PresentMode::Vsync ? swapchain_wait_ns : 0;
            const double cost_ms = ns_to_ms(now - frame_start_ns - excluded_ns);
            state.frame_times_ms.add(cost_ms);
            if (cost_ms <= state.scheduler.budget_ms())
            {
                ++state.frames_within_budget;
            }

            // Input takes priority when both asked, since it's the more specific cause.
            if (state.next_frame_from_input)
            {
                ++state.frames_from_input;
            }
            else if (state.frame_requested_by_event)
            {
                ++state.frames_from_window;
            }
            state.next_frame_from_input = false;
            state.frame_requested_by_event = false;

            if (state.pending_input_ns)
            {
                state.input_to_paint_ms.add(ns_to_ms(now - *state.pending_input_ns));
                state.pending_input_ns.reset();
            }
        }

        // Tracks the wall clock and CPU time of the part of an --idle-ms run after the window
        // has settled.
        class IdleCheck
        {
        public:
            IdleCheck(const Options& opts, std::uint64_t start_ns) :
                settle_ns_(start_ns + (kIdleSettleMs * kNanosPerMilliInt)),
                end_ns_(start_ns + (static_cast<std::uint64_t>(opts.idle_ms) * kNanosPerMilliInt))
            {
            }

            // Returns whether the check's time is up.
            bool update(std::uint64_t now_ns, LoopState& state)
            {
                if (!settled_ && now_ns >= settle_ns_)
                {
                    settled_ = true;
                    settled_presented_ = state.presented;
                    settled_from_input_ = state.frames_from_input;
                    settled_from_window_ = state.frames_from_window;
                    settled_cpu_ = std::clock();
                    settled_at_ns_ = now_ns;
                }
                if (now_ns < end_ns_)
                {
                    return false;
                }
                state.idle_frames = state.presented - settled_presented_;
                state.idle_frames_from_input = state.frames_from_input - settled_from_input_;
                state.idle_frames_from_window = state.frames_from_window - settled_from_window_;
                const double cpu_ms =
                    static_cast<double>(std::clock() - settled_cpu_) * kMillisPerSecond / CLOCKS_PER_SEC;
                state.idle_cpu_percent = kPercent * cpu_ms / ns_to_ms(now_ns - settled_at_ns_);
                return true;
            }

            [[nodiscard]] std::uint64_t next_boundary_ns() const noexcept { return settled_ ? end_ns_ : settle_ns_; }

        private:
            std::uint64_t settle_ns_;
            std::uint64_t end_ns_;
            bool settled_ = false;
            int settled_presented_ = 0;
            int settled_from_input_ = 0;
            int settled_from_window_ = 0;
            std::clock_t settled_cpu_ = 0;
            std::uint64_t settled_at_ns_ = 0;
        };

        // Rounds up, because waking just before something is due would only wait again.
        int ns_to_wait_ms(std::uint64_t ns)
        {
            return static_cast<int>((ns + kNanosPerMilliInt - 1) / kNanosPerMilliInt);
        }

        // How long the main loop can sleep: until the next frame or the next thing that ends
        // the run, whichever is first. -1 means until an event arrives.
        int wait_timeout_ms(const LoopState& state, std::uint64_t now_ns, std::optional<std::uint64_t> next_stop_ns)
        {
            std::optional<std::uint64_t> sleep_ns = state.scheduler.ns_until_due(now_ns);
            if (next_stop_ns)
            {
                const std::uint64_t until_stop = *next_stop_ns > now_ns ? *next_stop_ns - now_ns : 0;
                sleep_ns = sleep_ns ? std::min(*sleep_ns, until_stop) : until_stop;
            }
            return sleep_ns ? ns_to_wait_ms(*sleep_ns) : -1;
        }

        // Queues synthetic input at irregular intervals, then wakes the main loop to notice
        // that it's done. Joined before SDL shuts down.
        std::jthread start_input_injector(const Options& opts, std::atomic<bool>& done)
        {
            const int count = opts.inject_input;
            // A set interval varies by a quarter either way, like a person typing.
            const auto interval = static_cast<std::uint32_t>(opts.inject_interval_ms);
            const std::uint32_t minimum = interval > 0 ? interval - (interval / 4) : kInjectMinMs;
            const std::uint32_t spread = interval > 0 ? (interval / 2) + 1 : kInjectSpreadMs;
            return std::jthread{[count, minimum, spread, &done](const std::stop_token& stop)
                                {
                                    // A fixed sequence, so that runs compare fairly.
                                    constexpr std::uint32_t kMultiplier = 2654435761U;
                                    constexpr std::uint32_t kShift = 16;
                                    std::uint32_t random = 1;
                                    for (int i = 0; i < count && !stop.stop_requested(); ++i)
                                    {
                                        random *= kMultiplier;
                                        const std::uint32_t delay = minimum + ((random >> kShift) % spread);
                                        std::this_thread::sleep_for(std::chrono::milliseconds{delay});
                                        platform::post_test_input();
                                    }
                                    done.store(true);
                                    platform::post_wakeup();
                                }};
        }

        // Whether the run has reached its end, by whichever bound it has.
        bool run_complete(const Options& opts, const LoopState& state, const std::atomic<bool>& input_done,
                          std::uint64_t now_ns)
        {
            if (opts.frames > 0)
            {
                return state.presented >= opts.frames;
            }
            return opts.inject_input > 0 && input_done.load() && !state.pending_input_ns &&
                !state.scheduler.frame_due(now_ns);
        }

        // One frame's worth of work, once something has asked for a frame.
        bool draw_due_frame(const Renderer& renderer, platform::MainLoop& loop, LoopState& state, const Options& opts,
                            std::uint64_t frame_start_ns, bool& stall_injected)
        {
            const auto frame = render_frame(renderer, loop, state, opts.input_sampling);
            if (!frame)
            {
                state.exit_code = EXIT_FAILURE;
                return false;
            }

            inject_stall_once(opts, state.stalls, stall_injected);
            if (frame->presented)
            {
                record_presented_frame(state, opts, frame_start_ns, frame->swapchain_wait_ns);
                if (opts.resize_after_frames > 0 && state.presented == opts.resize_after_frames)
                {
                    renderer.window.set_size(opts.resize_width, opts.resize_height);
                }
            }

            // A bounded run is the test animation: every frame asks for the next one.
            if (opts.frames > 0)
            {
                state.scheduler.request_redraw(platform::now_ns());
            }
            return true;
        }

        void arm_timed_redraw(const Options& opts, LoopState& state, std::uint64_t now_ns)
        {
            if (opts.redraw_every_ms > 0 && state.scheduler.timed_requests_pending() == 0)
            {
                const auto every_ns = static_cast<std::uint64_t>(opts.redraw_every_ms) * kNanosPerMilliInt;
                std::ignore = state.scheduler.request_redraw_at(now_ns + every_ns);
            }
        }

        // Draws only what has been asked for, sleeping otherwise, until something ends the run.
        void run_main_loop(const Renderer& renderer, platform::MainLoop& loop, LoopState& state, const Options& opts)
        {
            const std::uint64_t start_ns = platform::now_ns();
            const std::optional<std::uint64_t> deadline_ns =
                opts.timeout_ms > 0 ? std::optional{start_ns + (opts.timeout_ms * kNanosPerMilliInt)} : std::nullopt;
            std::optional<IdleCheck> idle;
            if (opts.idle_ms > 0)
            {
                idle.emplace(opts, start_ns);
            }

            std::atomic<bool> input_done{false};
            std::jthread injector;
            if (opts.inject_input > 0)
            {
                injector = start_input_injector(opts, input_done);
            }

            bool stall_injected = false;
            state.scheduler.request_redraw(start_ns);

            while (true)
            {
                std::uint64_t now = platform::now_ns();
                arm_timed_redraw(opts, state, now);
                if (!state.scheduler.frame_due(now))
                {
                    const std::optional<std::uint64_t> next_stop = idle ? idle->next_boundary_ns() : deadline_ns;
                    loop.wait(wait_timeout_ms(state, now, next_stop));
                }

                const std::uint64_t iteration_start = platform::now_ns();
                state.stalls.begin_iteration();
                state.stalls.phase("handling events and messages");
                pump_main_loop(loop, renderer.window, state);
                if (state.quit)
                {
                    break;
                }

                if (state.scheduler.frame_due(platform::now_ns()) &&
                    !draw_due_frame(renderer, loop, state, opts, iteration_start, stall_injected))
                {
                    break;
                }
                state.stalls.end_iteration();

                now = platform::now_ns();
                if (state.quit || run_complete(opts, state, input_done, now) || (idle && idle->update(now, state)))
                {
                    break;
                }
                if (deadline_ns && now > *deadline_ns)
                {
                    STACKIDE_LOG_ERROR(LogCategory::Render, "timeout after {} ms with {} frame(s) presented",
                                       opts.timeout_ms, state.presented);
                    state.exit_code = EXIT_FAILURE;
                    break;
                }
            }
        }

        // An idle editor draws nothing it wasn't asked to, and barely runs. With timed redraws,
        // it draws those. On a live desktop the window system and the user can really ask for
        // frames, so those are allowed: it's a frame nobody asked for that's the defect.
        int check_idle(const Options& opts, const LoopState& state)
        {
            const int asked_for = state.idle_frames_from_input + state.idle_frames_from_window;
            const int unprompted = state.idle_frames - asked_for;
            std::println(
                stderr, "idle: {} frame(s) after settling ({} from input, {} from the window system), {:.2f}% cpu",
                state.idle_frames, state.idle_frames_from_input, state.idle_frames_from_window, state.idle_cpu_percent);
            if (opts.redraw_every_ms > 0)
            {
                const double expected = static_cast<double>(static_cast<std::uint64_t>(opts.idle_ms) - kIdleSettleMs) /
                    static_cast<double>(opts.redraw_every_ms);
                const double difference = static_cast<double>(unprompted) - expected;
                if (std::abs(difference) > expected * kTimedRedrawTolerance)
                {
                    STACKIDE_LOG_ERROR(LogCategory::Render, "expected about {:.0f} timed redraw(s), drew {}", expected,
                                       unprompted);
                    return EXIT_FAILURE;
                }
                return EXIT_SUCCESS;
            }
            if (unprompted != 0)
            {
                STACKIDE_LOG_ERROR(LogCategory::Render, "an idle window drew {} frame(s) nothing asked for",
                                   unprompted);
                return EXIT_FAILURE;
            }
            if (state.idle_cpu_percent > kIdleMaxCpuPercent)
            {
                STACKIDE_LOG_ERROR(LogCategory::Render, "idle cpu {:.2f}% exceeds {:.2f}%", state.idle_cpu_percent,
                                   kIdleMaxCpuPercent);
                return EXIT_FAILURE;
            }
            return EXIT_SUCCESS;
        }

        // The input half of the exit-time summary. Returns whether its gate passed.
        bool report_input_to_paint(const Options& opts, const LoopState& state)
        {
            if (state.input_to_paint_ms.size() == 0)
            {
                if (opts.inject_input == 0)
                {
                    return true;
                }
                STACKIDE_LOG_ERROR(LogCategory::Render, "input was injected but no frame reflected any of it");
                return false;
            }

            const std::vector<double> sorted = state.input_to_paint_ms.sorted();
            const double p99 = percentile(sorted, kP99);
            std::println(stderr, "input to paint: {} frame(s), p50={:.3f} ms p99={:.3f} ms max={:.3f} ms",
                         sorted.size(), percentile(sorted, kP50), p99, sorted.back());
            if (opts.assert_input_to_paint_under_ms && p99 > *opts.assert_input_to_paint_under_ms)
            {
                STACKIDE_LOG_ERROR(LogCategory::Render, "input to paint p99 {:.3f} ms exceeds gate {:.3f} ms", p99,
                                   *opts.assert_input_to_paint_under_ms);
                return false;
            }
            return true;
        }

        // The percentage of presented frames that met the budget.
        double within_budget_percent(const LoopState& state)
        {
            return state.presented > 0
                ? kPercent * static_cast<double>(state.frames_within_budget) / static_cast<double>(state.presented)
                : kPercent;
        }

        // Applies the frame gates, returning whether they all passed.
        bool check_frame_gates(const Options& opts, const LoopState& state, double p99)
        {
            if (opts.assert_p99_under_ms && p99 > *opts.assert_p99_under_ms)
            {
                STACKIDE_LOG_ERROR(LogCategory::Render, "p99 frame time {:.3f} ms exceeds gate {:.3f} ms", p99,
                                   *opts.assert_p99_under_ms);
                return false;
            }
            if (opts.assert_max_dropped_frames &&
                state.scheduler.missed_frames() > static_cast<std::uint64_t>(*opts.assert_max_dropped_frames))
            {
                STACKIDE_LOG_ERROR(LogCategory::Render, "dropped {} frame(s), gate allows {}",
                                   state.scheduler.missed_frames(), *opts.assert_max_dropped_frames);
                return false;
            }
            if (opts.assert_within_budget_percent && within_budget_percent(state) < *opts.assert_within_budget_percent)
            {
                STACKIDE_LOG_ERROR(LogCategory::Render, "{:.1f}% of frames within budget, gate requires {:.1f}%",
                                   within_budget_percent(state), *opts.assert_within_budget_percent);
                return false;
            }
            return true;
        }

        // Prints the exit-time summary and applies the gates. Returns the exit code.
        int report_and_gate(const Options& opts, const LoopState& state)
        {
            int exit_code = state.exit_code;

            if (exit_code == EXIT_SUCCESS && opts.frames > 0 && state.presented < opts.frames)
            {
                STACKIDE_LOG_ERROR(LogCategory::Render, "requested {} frame(s), presented {}", opts.frames,
                                   state.presented);
                exit_code = EXIT_FAILURE;
            }

            const std::vector<double> sorted = state.frame_times_ms.sorted();
            const double p50 = percentile(sorted, kP50);
            const double p99 = percentile(sorted, kP99);
            const double max_ms = sorted.empty() ? 0.0 : sorted.back();

            // The summary is program output, not a log line, so queued logs go first.
            log_flush();
            std::println(stderr,
                         "frames presented: {}, cpu frame time p50={:.3f} ms p99={:.3f} ms "
                         "max={:.3f} ms, dropped={}, within {:.2f} ms budget: {:.1f}%",
                         state.presented, p50, p99, max_ms, state.scheduler.missed_frames(),
                         state.scheduler.budget_ms(), within_budget_percent(state));

            if (!report_input_to_paint(opts, state) && exit_code == EXIT_SUCCESS)
            {
                exit_code = EXIT_FAILURE;
            }
            if (exit_code == EXIT_SUCCESS && !check_frame_gates(opts, state, p99))
            {
                exit_code = EXIT_FAILURE;
            }
            if (exit_code == EXIT_SUCCESS && opts.idle_ms > 0)
            {
                exit_code = check_idle(opts, state);
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

            LoopState state{kFallbackFrameBudgetMs,
                            opts.frames > 0 ? static_cast<std::size_t>(opts.frames) : kDefaultSampleCapacity};
            apply_refresh_rate(state, *window);
            STACKIDE_LOG_INFO(LogCategory::Platform, "display refresh rate: {:.2f} Hz, frame budget {:.2f} ms",
                              static_cast<double>(window->refresh_hz()), state.scheduler.budget_ms());

            const std::optional<QuadResources> quad = create_quad_resources(*device, swapchain->format());
            if (!quad)
            {
                return render::gpu_unavailable_code(opts);
            }

            // After everything the core could need, and before the loop that drains it.
            Core core{[] { platform::post_wakeup(); }};
            platform::MainLoop loop{core.to_ui()};
            const Renderer renderer{
                .device = *device,
                .swapchain = *swapchain,
                .window = *window,
                .quad = *quad,
            };

            // Only a run measuring cadence has anything for warmup frames to protect.
            constexpr int kWarmupFrames = 20;
            if (opts.frames > 0 && !run_warmup_frames(renderer, loop, state, kWarmupFrames))
            {
                return EXIT_FAILURE;
            }
            if (!state.quit)
            {
                run_main_loop(renderer, loop, state, opts);
            }
            return report_and_gate(opts, state);
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
