#include <stackide/modes.hpp>

#include <stackide/constants.hpp>
#include <stackide/quad_resources.hpp>
#include <stackide/scope_exit.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <print>
#include <vector>

namespace stackide {
namespace {

using Clock = std::chrono::steady_clock;

// Nearest-rank percentile over an already-sorted sample.
double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    const auto rank = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1));
    return sorted.at(rank);
}

// M1 doc's fixed measurement definition: a frame interval over 1.5x the
// expected refresh interval counts as dropped. Not a CTest-tunable knob.
constexpr double kDropIntervalMultiplier = 1.5;

// Only used when --frames is unbounded; a bounded run reserves exactly that
// many slots instead.
constexpr std::size_t kDefaultFrameCapacityReserve = 4096;

constexpr Uint32 kQuadIndexCount = 6;
constexpr Uint32 kOccludedFrameDelayMs = 16;

// Shrinks whichever NDC axis is longer so the (square) checkerboard quad
// doesn't stretch to fill a non-square window.
void compute_letterbox_scale(SDL_Window* window, std::array<float, 2>& scale) {
    int w = 0;
    int h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w <= 0 || h <= 0) {
        scale = {1.0F, 1.0F};
        return;
    }
    const double aspect = static_cast<double>(w) / static_cast<double>(h);
    if (aspect >= 1.0) {
        scale = {static_cast<float>(1.0 / aspect), 1.0F};
    } else {
        scale = {1.0F, static_cast<float>(aspect)};
    }
}

// True if a quit/escape event was seen in the pending event queue. Drains
// the whole queue either way.
bool poll_quit_requested() {
    bool quit = false;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT ||
            (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) {
            quit = true;
        }
    }
    return quit;
}

// Renders and submits one frame. Returns true if a frame reached the
// swapchain, false if the window was minimized/occluded (not an error, just
// nothing to present), or nullopt on a real failure.
std::optional<bool> render_frame(SDL_GPUDevice* gpu, SDL_Window* window,
                                 const QuadResources& quad) {
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu);
    if (cmd == nullptr) {
        std::println(stderr, "SDL_AcquireGPUCommandBuffer failed: {}", SDL_GetError());
        return std::nullopt;
    }

    SDL_GPUTexture* swapchain = nullptr;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swapchain, nullptr, nullptr)) {
        std::println(stderr, "SDL_WaitAndAcquireGPUSwapchainTexture failed: {}", SDL_GetError());
        // Nothing was acquired, so cancel rather than submit.
        SDL_CancelGPUCommandBuffer(cmd);
        return std::nullopt;
    }

    if (swapchain == nullptr) {
        // Minimized or occluded: not an error, but no frame was presented.
        SDL_SubmitGPUCommandBuffer(cmd);
        SDL_Delay(kOccludedFrameDelayMs);
        return false;
    }

    SDL_GPUColorTargetInfo target{};
    target.texture = swapchain;
    target.clear_color = clear_color();
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
    if (pass == nullptr) {
        std::println(stderr, "SDL_BeginGPURenderPass failed: {}", SDL_GetError());
        // A swapchain texture was acquired, so cancel is illegal here.
        SDL_SubmitGPUCommandBuffer(cmd);
        return std::nullopt;
    }

    SDL_BindGPUGraphicsPipeline(pass, quad.pipeline);

    const SDL_GPUBufferBinding vb_binding{.buffer = quad.vertex_buffer, .offset = 0};
    SDL_BindGPUVertexBuffers(pass, 0, &vb_binding, 1);

    const SDL_GPUBufferBinding ib_binding{.buffer = quad.index_buffer, .offset = 0};
    SDL_BindGPUIndexBuffer(pass, &ib_binding, SDL_GPU_INDEXELEMENTSIZE_16BIT);

    const SDL_GPUTextureSamplerBinding tex_binding{.texture = quad.texture,
                                                   .sampler = quad.sampler,};
    SDL_BindGPUFragmentSamplers(pass, 0, &tex_binding, 1);

    std::array<float, 2> scale{};
    compute_letterbox_scale(window, scale);
    SDL_PushGPUVertexUniformData(cmd, 0, scale.data(), sizeof(scale));

    SDL_DrawGPUIndexedPrimitives(pass, kQuadIndexCount, 1, 0, 0, 0);

    SDL_EndGPURenderPass(pass);
    SDL_SubmitGPUCommandBuffer(cmd);
    return true;
}

// Absorbs the one-time swapchain/compositor negotiation cost that the first
// several presented frames pay on a freshly-mapped window (observed: one
// frame stalling ~2.8x the refresh interval, consistently within the first
// ~10 frames, regardless of how long the process waits beforehand -- it is
// tied to frame count, not wall-clock time). Returns false on a render
// failure.
bool run_warmup_frames(SDL_GPUDevice* gpu, SDL_Window* window, const QuadResources& quad,
                       int count) {
    for (int i = 0; i < count; ++i) {
        if (poll_quit_requested()) {
            return true;
        }
        if (!render_frame(gpu, window, quad)) {
            return false;
        }
    }
    return true;
}

struct FrameStats {
    int exit_code = EXIT_SUCCESS;
    int presented = 0;
    int dropped_frames = 0;
    std::vector<double> frame_times_ms;
};

// Runs the measured loop: presents frames until quit, --frames is reached,
// or --timeout-ms elapses, tracking per-frame timing and dropped-frame
// counts along the way.
FrameStats run_measured_loop(SDL_GPUDevice* gpu, SDL_Window* window, const QuadResources& quad,
                             const Options& opts, double expected_interval_ms) {
    FrameStats stats;
    stats.frame_times_ms.reserve(opts.frames > 0 ? static_cast<std::size_t>(opts.frames)
                                                  : kDefaultFrameCapacityReserve);

    auto last = Clock::now();
    const Uint64 started_ms = SDL_GetTicks();
    bool running = true;

    while (running) {
        if (poll_quit_requested()) {
            break;
        }

        const std::optional<bool> result = render_frame(gpu, window, quad);
        if (!result) {
            stats.exit_code = EXIT_FAILURE;
            break;
        }

        if (*result) {
            ++stats.presented;

            if (opts.resize_after_frames > 0 && stats.presented == opts.resize_after_frames) {
                SDL_SetWindowSize(window, opts.resize_width, opts.resize_height);
            }

            const auto now = Clock::now();
            const double delta_ms = std::chrono::duration<double, std::milli>(now - last).count();
            if (expected_interval_ms > 0.0 && stats.presented > 1 &&
                delta_ms > kDropIntervalMultiplier * expected_interval_ms) {
                ++stats.dropped_frames;
            }
            stats.frame_times_ms.push_back(delta_ms);
            last = now;

            if (opts.frames > 0 && stats.presented >= opts.frames) {
                running = false;
            }
        }

        if (opts.timeout_ms > 0 && SDL_GetTicks() - started_ms > opts.timeout_ms) {
            std::println(stderr, "timeout after {} ms with {} frame(s) presented",
                         opts.timeout_ms, stats.presented);
            stats.exit_code = EXIT_FAILURE;
            break;
        }
    }

    return stats;
}

// Claims the swapchain's composition and present mode, printing its own
// error/skip diagnostics. Returns nullopt if this device doesn't support
// what's needed.
std::optional<SDL_GPUPresentMode> configure_swapchain(SDL_GPUDevice* gpu, SDL_Window* window,
                                                       const Options& opts) {
    // SDR_LINEAR is required (decision 3), not a soft preference: never fall
    // back to plain SDR composition if this device doesn't support it.
    if (!SDL_WindowSupportsGPUSwapchainComposition(gpu, window,
                                                    SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR)) {
        std::println(stderr, "SDR_LINEAR swapchain composition not supported on this device");
        return std::nullopt;
    }
    const SDL_GPUPresentMode present_mode = opts.present_mode == PresentMode::Immediate
                                                 ? SDL_GPU_PRESENTMODE_IMMEDIATE
                                                 : SDL_GPU_PRESENTMODE_VSYNC;
    // VSYNC is guaranteed supported by every SDL_GPU backend; IMMEDIATE is not.
    if (present_mode != SDL_GPU_PRESENTMODE_VSYNC &&
        !SDL_WindowSupportsGPUPresentMode(gpu, window, present_mode)) {
        std::println(stderr, "immediate present mode not supported on this device");
        return std::nullopt;
    }
    if (!SDL_SetGPUSwapchainParameters(gpu, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR,
                                        present_mode)) {
        std::println(stderr, "SDL_SetGPUSwapchainParameters failed: {}", SDL_GetError());
        return std::nullopt;
    }
    return present_mode;
}

// The live refresh rate, or 0.0F if the display or its current mode
// couldn't be queried.
float detect_refresh_hz(SDL_Window* window) {
    float refresh_hz = 0.0F;
    if (const SDL_DisplayID display_id = SDL_GetDisplayForWindow(window); display_id != 0) {
        if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display_id);
            mode != nullptr) {
            refresh_hz = mode->refresh_rate;
        }
    }
    return refresh_hz;
}

// Prints the exit-time summary and applies items 6/7's gates. Returns the
// (possibly updated) exit code.
int report_and_gate(const Options& opts, const FrameStats& stats, double expected_interval_ms) {
    int exit_code = stats.exit_code;

    if (exit_code == EXIT_SUCCESS && opts.frames > 0 && stats.presented < opts.frames) {
        std::println(stderr, "requested {} frame(s), presented {}", opts.frames, stats.presented);
        exit_code = EXIT_FAILURE;
    }

    std::vector<double> sorted = stats.frame_times_ms;
    std::ranges::sort(sorted);
    const double p50 = percentile(sorted, 0.50);
    const double p99 = percentile(sorted, 0.99);
    const double max_ms = sorted.empty() ? 0.0 : sorted.back();

    if (expected_interval_ms > 0.0) {
        std::println(stderr,
                     "frames presented: {}, cpu frame time p50={:.3f} ms p99={:.3f} ms "
                     "max={:.3f} ms, dropped={}",
                     stats.presented, p50, p99, max_ms, stats.dropped_frames);
    } else {
        std::println(stderr,
                     "frames presented: {}, cpu frame time p50={:.3f} ms p99={:.3f} ms "
                     "max={:.3f} ms, dropped=n/a (refresh rate unknown)",
                     stats.presented, p50, p99, max_ms);
    }

    if (exit_code == EXIT_SUCCESS && opts.assert_p99_under_ms && p99 > *opts.assert_p99_under_ms) {
        std::println(stderr, "p99 frame time {:.3f} ms exceeds gate {:.3f} ms", p99,
                     *opts.assert_p99_under_ms);
        exit_code = EXIT_FAILURE;
    }
    if (exit_code == EXIT_SUCCESS && opts.assert_max_dropped_frames &&
        stats.dropped_frames > *opts.assert_max_dropped_frames) {
        std::println(stderr, "dropped {} frame(s), gate allows {}", stats.dropped_frames,
                     *opts.assert_max_dropped_frames);
        exit_code = EXIT_FAILURE;
    }

    return exit_code;
}

} // namespace

int run_windowed(const Options& opts) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::println(stderr, "SDL_Init failed: {}", SDL_GetError());
        return EXIT_FAILURE;
    }
    ScopeExit quit{[] { SDL_Quit(); }};

    SDL_Window* window = SDL_CreateWindow("stackide", kWindowWidth, kWindowHeight,
                                          SDL_WINDOW_RESIZABLE);
    if (window == nullptr) {
        std::println(stderr, "SDL_CreateWindow failed: {}", SDL_GetError());
        return EXIT_FAILURE;
    }
    ScopeExit destroy_window{[window] { SDL_DestroyWindow(window); }};

    SDL_GPUDevice* gpu =
        SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, opts.gpu_debug, nullptr);
    if (gpu == nullptr) {
        std::println(stderr, "SDL_CreateGPUDevice failed: {}", SDL_GetError());
        return opts.allow_skip ? kExitSkip : EXIT_FAILURE;
    }
    ScopeExit destroy_gpu{[gpu] { SDL_DestroyGPUDevice(gpu); }};

    if (!SDL_ClaimWindowForGPUDevice(gpu, window)) {
        std::println(stderr, "SDL_ClaimWindowForGPUDevice failed: {}", SDL_GetError());
        return opts.allow_skip ? kExitSkip : EXIT_FAILURE;
    }
    ScopeExit release_window{[gpu, window] { SDL_ReleaseWindowFromGPUDevice(gpu, window); }};

    const std::optional<SDL_GPUPresentMode> present_mode = configure_swapchain(gpu, window, opts);
    if (!present_mode) {
        return opts.allow_skip ? kExitSkip : EXIT_FAILURE;
    }

    std::println(stderr, "gpu driver: {}", SDL_GetGPUDeviceDriver(gpu));
    const SDL_GPUTextureFormat swapchain_format = SDL_GetGPUSwapchainTextureFormat(gpu, window);
    std::println(stderr, "swapchain format: {}", static_cast<int>(swapchain_format));

    const float refresh_hz = detect_refresh_hz(window);
    std::println(stderr, "display refresh rate: {:.2f} Hz", static_cast<double>(refresh_hz));
    const double expected_interval_ms =
        refresh_hz > 0.0F ? 1000.0 / static_cast<double>(refresh_hz) : 0.0;

    std::optional<QuadResources> quad = create_quad_resources(gpu, swapchain_format);
    if (!quad) {
        return opts.allow_skip ? kExitSkip : EXIT_FAILURE;
    }
    ScopeExit destroy_quad{[gpu, res = *quad] { destroy_quad_resources(gpu, res); }};

    constexpr int kWarmupFrames = 20;
    if (!run_warmup_frames(gpu, window, *quad, kWarmupFrames)) {
        return EXIT_FAILURE;
    }

    const FrameStats stats = run_measured_loop(gpu, window, *quad, opts, expected_interval_ms);
    return report_and_gate(opts, stats, expected_interval_ms);
}

} // namespace stackide
