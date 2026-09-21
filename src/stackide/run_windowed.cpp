#include <stackide/modes.hpp>

#include <stackide/constants.hpp>
#include <stackide/quad_resources.hpp>
#include <stackide/scope_exit.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <print>
#include <vector>

namespace stackide {
namespace {

// Nearest-rank percentile over an already-sorted sample.
double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    const auto rank = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1));
    return sorted[rank];
}

// M1 doc's fixed measurement definition: a frame interval over 1.5x the
// expected refresh interval counts as dropped. Not a CTest-tunable knob.
constexpr double kDropIntervalMultiplier = 1.5;

} // namespace

int run_windowed(const Options& opts) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::println(stderr, "SDL_Init failed: {}", SDL_GetError());
        return EXIT_FAILURE;
    }
    ScopeExit quit{[] { SDL_Quit(); }};

    SDL_Window* window = SDL_CreateWindow("stackide", kWindowWidth, kWindowHeight,
                                          SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::println(stderr, "SDL_CreateWindow failed: {}", SDL_GetError());
        return EXIT_FAILURE;
    }
    ScopeExit destroy_window{[window] { SDL_DestroyWindow(window); }};

    SDL_GPUDevice* gpu =
        SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, opts.gpu_debug, nullptr);
    if (!gpu) {
        std::println(stderr, "SDL_CreateGPUDevice failed: {}", SDL_GetError());
        return opts.allow_skip ? kExitSkip : EXIT_FAILURE;
    }
    ScopeExit destroy_gpu{[gpu] { SDL_DestroyGPUDevice(gpu); }};

    if (!SDL_ClaimWindowForGPUDevice(gpu, window)) {
        std::println(stderr, "SDL_ClaimWindowForGPUDevice failed: {}", SDL_GetError());
        return opts.allow_skip ? kExitSkip : EXIT_FAILURE;
    }
    ScopeExit release_window{[gpu, window] { SDL_ReleaseWindowFromGPUDevice(gpu, window); }};

    // SDR_LINEAR is required (decision 3), not a soft preference: never fall
    // back to plain SDR composition if this device doesn't support it.
    if (!SDL_WindowSupportsGPUSwapchainComposition(gpu, window,
                                                    SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR)) {
        std::println(stderr, "SDR_LINEAR swapchain composition not supported on this device");
        return opts.allow_skip ? kExitSkip : EXIT_FAILURE;
    }
    const SDL_GPUPresentMode present_mode = opts.present_mode == PresentMode::Immediate
                                                 ? SDL_GPU_PRESENTMODE_IMMEDIATE
                                                 : SDL_GPU_PRESENTMODE_VSYNC;
    // VSYNC is guaranteed supported by every SDL_GPU backend; IMMEDIATE is not.
    if (present_mode != SDL_GPU_PRESENTMODE_VSYNC &&
        !SDL_WindowSupportsGPUPresentMode(gpu, window, present_mode)) {
        std::println(stderr, "immediate present mode not supported on this device");
        return opts.allow_skip ? kExitSkip : EXIT_FAILURE;
    }
    if (!SDL_SetGPUSwapchainParameters(gpu, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR,
                                        present_mode)) {
        std::println(stderr, "SDL_SetGPUSwapchainParameters failed: {}", SDL_GetError());
        return opts.allow_skip ? kExitSkip : EXIT_FAILURE;
    }

    std::println(stderr, "gpu driver: {}", SDL_GetGPUDeviceDriver(gpu));
    const SDL_GPUTextureFormat swapchain_format = SDL_GetGPUSwapchainTextureFormat(gpu, window);
    std::println(stderr, "swapchain format: {}", static_cast<int>(swapchain_format));

    float refresh_hz = 0.0f;
    if (const SDL_DisplayID display_id = SDL_GetDisplayForWindow(window); display_id != 0) {
        if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display_id)) {
            refresh_hz = mode->refresh_rate;
        }
    }
    std::println(stderr, "display refresh rate: {:.2f} Hz", static_cast<double>(refresh_hz));
    const double expected_interval_ms =
        refresh_hz > 0.0f ? 1000.0 / static_cast<double>(refresh_hz) : 0.0;

    std::optional<QuadResources> quad = create_quad_resources(gpu, swapchain_format);
    if (!quad) {
        return opts.allow_skip ? kExitSkip : EXIT_FAILURE;
    }
    ScopeExit destroy_quad{[gpu, res = *quad] { destroy_quad_resources(gpu, res); }};

    // Renders and submits one frame. Returns true if a frame reached the
    // swapchain, false if the window was minimized/occluded (not an error,
    // just nothing to present), or nullopt on a real failure.
    const auto render_frame = [&]() -> std::optional<bool> {
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu);
        if (!cmd) {
            std::println(stderr, "SDL_AcquireGPUCommandBuffer failed: {}", SDL_GetError());
            return std::nullopt;
        }

        SDL_GPUTexture* swapchain = nullptr;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swapchain, nullptr, nullptr)) {
            std::println(stderr, "SDL_WaitAndAcquireGPUSwapchainTexture failed: {}",
                         SDL_GetError());
            // Nothing was acquired, so cancel rather than submit.
            SDL_CancelGPUCommandBuffer(cmd);
            return std::nullopt;
        }

        if (!swapchain) {
            // Minimized or occluded: not an error, but no frame was presented.
            SDL_SubmitGPUCommandBuffer(cmd);
            SDL_Delay(16);
            return false;
        }

        SDL_GPUColorTargetInfo target{};
        target.texture = swapchain;
        target.clear_color = clear_color();
        target.load_op = SDL_GPU_LOADOP_CLEAR;
        target.store_op = SDL_GPU_STOREOP_STORE;

        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
        if (!pass) {
            std::println(stderr, "SDL_BeginGPURenderPass failed: {}", SDL_GetError());
            // A swapchain texture was acquired, so cancel is illegal here.
            SDL_SubmitGPUCommandBuffer(cmd);
            return std::nullopt;
        }

        SDL_BindGPUGraphicsPipeline(pass, quad->pipeline);

        const SDL_GPUBufferBinding vb_binding{quad->vertex_buffer, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vb_binding, 1);

        const SDL_GPUBufferBinding ib_binding{quad->index_buffer, 0};
        SDL_BindGPUIndexBuffer(pass, &ib_binding, SDL_GPU_INDEXELEMENTSIZE_16BIT);

        const SDL_GPUTextureSamplerBinding tex_binding{quad->texture, quad->sampler};
        SDL_BindGPUFragmentSamplers(pass, 0, &tex_binding, 1);

        // Identity for now: real aspect-preserving letterboxing lands
        // with resize handling, which is what this uniform exists for.
        constexpr float kIdentityScale[2] = {1.0f, 1.0f};
        SDL_PushGPUVertexUniformData(cmd, 0, kIdentityScale, sizeof(kIdentityScale));

        SDL_DrawGPUIndexedPrimitives(pass, 6, 1, 0, 0, 0);

        SDL_EndGPURenderPass(pass);
        SDL_SubmitGPUCommandBuffer(cmd);
        return true;
    };

    // The first several swapchain acquisitions on a freshly-mapped window pay
    // a one-time compositor/swapchain negotiation cost (observed: one frame
    // stalling ~2.8x the refresh interval, consistently within the first ~10
    // frames, regardless of how long the process waits beforehand -- it is
    // tied to frame count, not wall-clock time). Absorb it here, unmeasured,
    // so it doesn't register as a dropped frame or skew frame timing.
    constexpr int kWarmupFrames = 20;
    bool running = true;
    for (int i = 0; i < kWarmupFrames && running; ++i) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) {
                running = false;
            }
        }
        if (!running) {
            break;
        }
        if (!render_frame()) {
            return EXIT_FAILURE;
        }
    }

    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    std::vector<double> frame_times_ms;
    // Reserved upfront: a mid-loop reallocation costs enough (first-touch
    // page faults on the newly grown heap) to itself register as a dropped
    // frame in the very measurement this vector exists to take.
    frame_times_ms.reserve(opts.frames > 0 ? static_cast<std::size_t>(opts.frames) : 4096);
    int dropped_frames = 0;

    const Uint64 started_ms = SDL_GetTicks();
    int exit_code = EXIT_SUCCESS;
    int presented = 0;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN &&
                       event.key.key == SDLK_ESCAPE) {
                running = false;
            }
        }
        if (!running) break;

        const std::optional<bool> result = render_frame();
        if (!result) {
            exit_code = EXIT_FAILURE;
            break;
        }

        if (*result) {
            // Only a frame that actually reached the swapchain counts.
            ++presented;

            const auto now = clock::now();
            const double delta_ms = std::chrono::duration<double, std::milli>(now - last).count();
            if (expected_interval_ms > 0.0 && presented > 1 &&
                delta_ms > kDropIntervalMultiplier * expected_interval_ms) {
                ++dropped_frames;
            }
            frame_times_ms.push_back(delta_ms);
            last = now;

            if (opts.frames > 0 && presented >= opts.frames) {
                running = false;
            }
        }

        if (opts.timeout_ms > 0 && SDL_GetTicks() - started_ms > opts.timeout_ms) {
            std::println(stderr,
                         "timeout after {} ms with {} frame(s) presented",
                         opts.timeout_ms, presented);
            exit_code = EXIT_FAILURE;
            break;
        }
    }

    if (exit_code == EXIT_SUCCESS && opts.frames > 0 && presented < opts.frames) {
        std::println(stderr, "requested {} frame(s), presented {}", opts.frames, presented);
        exit_code = EXIT_FAILURE;
    }

    std::ranges::sort(frame_times_ms);
    const double p50 = percentile(frame_times_ms, 0.50);
    const double p99 = percentile(frame_times_ms, 0.99);
    const double max_ms = frame_times_ms.empty() ? 0.0 : frame_times_ms.back();

    if (expected_interval_ms > 0.0) {
        std::println(stderr,
                     "frames presented: {}, cpu frame time p50={:.3f} ms p99={:.3f} ms "
                     "max={:.3f} ms, dropped={}",
                     presented, p50, p99, max_ms, dropped_frames);
    } else {
        std::println(stderr,
                     "frames presented: {}, cpu frame time p50={:.3f} ms p99={:.3f} ms "
                     "max={:.3f} ms, dropped=n/a (refresh rate unknown)",
                     presented, p50, p99, max_ms);
    }

    if (exit_code == EXIT_SUCCESS && opts.assert_p99_under_ms && p99 > *opts.assert_p99_under_ms) {
        std::println(stderr, "p99 frame time {:.3f} ms exceeds gate {:.3f} ms", p99,
                     *opts.assert_p99_under_ms);
        exit_code = EXIT_FAILURE;
    }
    if (exit_code == EXIT_SUCCESS && opts.assert_max_dropped_frames &&
        dropped_frames > *opts.assert_max_dropped_frames) {
        std::println(stderr, "dropped {} frame(s), gate allows {}", dropped_frames,
                     *opts.assert_max_dropped_frames);
        exit_code = EXIT_FAILURE;
    }

    return exit_code;
}

} // namespace stackide
