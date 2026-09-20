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

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <print>
#include <string_view>
#include <utility>

namespace stackide {
namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

inline constexpr int kWindowWidth = 1280;
inline constexpr int kWindowHeight = 800;

inline constexpr int kOffscreenWidth = 64;
inline constexpr int kOffscreenHeight = 64;

// Swapchain and offscreen target both use an sRGB-encoded format (decision 3
// in docs/buildouts/M1-textured-quad.md): the eventual glyph atlas needs
// linear-space alpha blending for antialiased edges, so this is load-bearing
// now rather than retrofitted once text rendering exists.
inline constexpr SDL_GPUTextureFormat kOffscreenColorFormat =
    SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;

// The clear color is defined in 8-bit terms so the offscreen mode can assert
// exact byte values. Float form is derived, never the other way around.
inline constexpr std::uint8_t kClearR = 0x1A;
inline constexpr std::uint8_t kClearG = 0x1A;
inline constexpr std::uint8_t kClearB = 0x1F;
inline constexpr std::uint8_t kClearA = 0xFF;

// Drivers differ by one ULP in unorm rounding. Tolerate it, nothing more.
inline constexpr int kChannelTolerance = 1;

// CTest convention for "skipped" (see SKIP_RETURN_CODE).
inline constexpr int kExitSkip = 77;

inline constexpr Uint64 kDefaultTimeoutMs = 10'000;

// An _SRGB texture format auto-encodes a shader's linear float output to its
// sRGB-encoded byte storage. To keep the clear assertion's expected bytes
// (kClearR/G/B/A) exactly as they are, the clear color fed to SDL has to be
// the *decoded* linear value that round-trips back through that encode.
float srgb_decode(std::uint8_t v) noexcept {
    const float c = static_cast<float>(v) / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

SDL_FColor clear_color() noexcept {
    return SDL_FColor{srgb_decode(kClearR), srgb_decode(kClearG), srgb_decode(kClearB),
                       srgb_decode(kClearA)};
}

// ---------------------------------------------------------------------------
// Scope guard
// ---------------------------------------------------------------------------

template <class F>
class ScopeExit {
public:
    explicit ScopeExit(F fn) noexcept : fn_(std::move(fn)) {}
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ~ScopeExit() { if (armed_) fn_(); }
    void release() noexcept { armed_ = false; }

private:
    F fn_;
    bool armed_ = true;
};

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

enum class Mode { Windowed, Offscreen, Headless };

struct Options {
    Mode mode = Mode::Windowed;
    int frames = 0;                       // 0 means run until quit
    Uint64 timeout_ms = kDefaultTimeoutMs; // 0 disables the deadline
    bool allow_skip = false;
    bool gpu_debug = false;
};

void print_usage() {
    std::println(stderr,
                 "usage: stackide [--frames N] [--offscreen] [--headless] "
                 "[--timeout-ms N] [--allow-skip] [--gpu-debug]");
}

bool parse_int(std::string_view text, int& out) {
    const auto* first = text.data();
    const auto* last = first + text.size();
    const auto result = std::from_chars(first, last, out);
    return result.ec == std::errc{} && result.ptr == last;
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

// ---------------------------------------------------------------------------
// Offscreen: GPU device with no window. Clears an owned texture, downloads it,
// and asserts the pixels. This is the mode that actually verifies rendering.
// ---------------------------------------------------------------------------

bool pixel_matches(const std::uint8_t* px) {
    const auto near = [](std::uint8_t got, std::uint8_t want) {
        return std::abs(static_cast<int>(got) - static_cast<int>(want)) <= kChannelTolerance;
    };
    return near(px[0], kClearR) && near(px[1], kClearG) &&
           near(px[2], kClearB) && near(px[3], kClearA);
}

int run_offscreen(const Options& opts) {
    // No window is created, so the dummy video driver is sufficient and the
    // binary runs on a CI host with no display server.
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::println(stderr, "SDL_Init failed: {}", SDL_GetError());
        return EXIT_FAILURE;
    }
    ScopeExit quit{[] { SDL_Quit(); }};

    SDL_GPUDevice* gpu =
        SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, opts.gpu_debug, nullptr);
    if (!gpu) {
        std::println(stderr, "SDL_CreateGPUDevice failed: {}", SDL_GetError());
        return opts.allow_skip ? kExitSkip : EXIT_FAILURE;
    }
    ScopeExit destroy_gpu{[gpu] { SDL_DestroyGPUDevice(gpu); }};

    std::println(stderr, "offscreen: gpu driver = {}", SDL_GetGPUDeviceDriver(gpu));

    constexpr auto kFormat = kOffscreenColorFormat;
    if (!SDL_GPUTextureSupportsFormat(gpu, kFormat, SDL_GPU_TEXTURETYPE_2D,
                                      SDL_GPU_TEXTUREUSAGE_COLOR_TARGET)) {
        std::println(stderr, "R8G8B8A8_UNORM_SRGB color target unsupported on this device");
        return EXIT_FAILURE;
    }

    SDL_GPUTextureCreateInfo tex_info{};
    tex_info.type = SDL_GPU_TEXTURETYPE_2D;
    tex_info.format = kFormat;
    tex_info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    tex_info.width = kOffscreenWidth;
    tex_info.height = kOffscreenHeight;
    tex_info.layer_count_or_depth = 1;
    tex_info.num_levels = 1;
    tex_info.sample_count = SDL_GPU_SAMPLECOUNT_1;

    SDL_GPUTexture* color = SDL_CreateGPUTexture(gpu, &tex_info);
    if (!color) {
        std::println(stderr, "SDL_CreateGPUTexture failed: {}", SDL_GetError());
        return EXIT_FAILURE;
    }
    ScopeExit destroy_tex{[gpu, color] { SDL_ReleaseGPUTexture(gpu, color); }};

    constexpr Uint32 kPixelBytes = 4;
    constexpr Uint32 kBufferSize = kOffscreenWidth * kOffscreenHeight * kPixelBytes;

    SDL_GPUTransferBufferCreateInfo tb_info{};
    tb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tb_info.size = kBufferSize;

    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(gpu, &tb_info);
    if (!transfer) {
        std::println(stderr, "SDL_CreateGPUTransferBuffer failed: {}", SDL_GetError());
        return EXIT_FAILURE;
    }
    ScopeExit destroy_tb{[gpu, transfer] { SDL_ReleaseGPUTransferBuffer(gpu, transfer); }};

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu);
    if (!cmd) {
        std::println(stderr, "SDL_AcquireGPUCommandBuffer failed: {}", SDL_GetError());
        return EXIT_FAILURE;
    }

    SDL_GPUColorTargetInfo target{};
    target.texture = color;
    target.clear_color = clear_color();
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
    if (!pass) {
        std::println(stderr, "SDL_BeginGPURenderPass failed: {}", SDL_GetError());
        SDL_CancelGPUCommandBuffer(cmd);
        return EXIT_FAILURE;
    }
    SDL_EndGPURenderPass(pass);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    if (!copy) {
        std::println(stderr, "SDL_BeginGPUCopyPass failed: {}", SDL_GetError());
        SDL_CancelGPUCommandBuffer(cmd);
        return EXIT_FAILURE;
    }

    SDL_GPUTextureRegion region{};
    region.texture = color;
    region.w = kOffscreenWidth;
    region.h = kOffscreenHeight;
    region.d = 1;

    SDL_GPUTextureTransferInfo destination{};
    destination.transfer_buffer = transfer;
    destination.offset = 0;
    destination.pixels_per_row = kOffscreenWidth;
    destination.rows_per_layer = kOffscreenHeight;

    SDL_DownloadFromGPUTexture(copy, &region, &destination);
    SDL_EndGPUCopyPass(copy);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence) {
        std::println(stderr, "SDL_SubmitGPUCommandBufferAndAcquireFence failed: {}",
                     SDL_GetError());
        return EXIT_FAILURE;
    }
    if (!SDL_WaitForGPUFences(gpu, true, &fence, 1)) {
        std::println(stderr, "SDL_WaitForGPUFences failed: {}", SDL_GetError());
        SDL_ReleaseGPUFence(gpu, fence);
        return EXIT_FAILURE;
    }
    SDL_ReleaseGPUFence(gpu, fence);

    auto* pixels = static_cast<std::uint8_t*>(
        SDL_MapGPUTransferBuffer(gpu, transfer, false));
    if (!pixels) {
        std::println(stderr, "SDL_MapGPUTransferBuffer failed: {}", SDL_GetError());
        return EXIT_FAILURE;
    }
    ScopeExit unmap{[gpu, transfer] { SDL_UnmapGPUTransferBuffer(gpu, transfer); }};

    // Check a corner and the center: catches a clear that only partially lands.
    constexpr Uint32 kCenter =
        (kOffscreenHeight / 2 * kOffscreenWidth + kOffscreenWidth / 2) * kPixelBytes;

    for (const Uint32 offset : {Uint32{0}, kCenter}) {
        const std::uint8_t* px = pixels + offset;
        if (!pixel_matches(px)) {
            std::println(stderr,
                         "pixel mismatch at byte {}: got {:02X}{:02X}{:02X}{:02X}, "
                         "expected {:02X}{:02X}{:02X}{:02X}",
                         offset, px[0], px[1], px[2], px[3],
                         kClearR, kClearG, kClearB, kClearA);
            return EXIT_FAILURE;
        }
    }

    std::println(stderr, "offscreen: clear verified at {}x{}", kOffscreenWidth,
                 kOffscreenHeight);
    return EXIT_SUCCESS;
}

// ---------------------------------------------------------------------------
// Windowed: the real render loop, optionally bounded by a frame count.
// ---------------------------------------------------------------------------

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
    // Present mode is fixed at VSYNC until --present-mode lands; only the
    // composition is being set explicitly here.
    if (!SDL_SetGPUSwapchainParameters(gpu, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR,
                                        SDL_GPU_PRESENTMODE_VSYNC)) {
        std::println(stderr, "SDL_SetGPUSwapchainParameters failed: {}", SDL_GetError());
        return opts.allow_skip ? kExitSkip : EXIT_FAILURE;
    }

    std::println(stderr, "gpu driver: {}", SDL_GetGPUDeviceDriver(gpu));
    std::println(stderr, "swapchain format: {}",
                 static_cast<int>(SDL_GetGPUSwapchainTextureFormat(gpu, window)));

    using clock = std::chrono::steady_clock;
    auto last = clock::now();

    const Uint64 started_ms = SDL_GetTicks();
    int exit_code = EXIT_SUCCESS;
    int presented = 0;
    bool running = true;

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

        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu);
        if (!cmd) {
            std::println(stderr, "SDL_AcquireGPUCommandBuffer failed: {}", SDL_GetError());
            exit_code = EXIT_FAILURE;
            break;
        }

        SDL_GPUTexture* swapchain = nullptr;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swapchain,
                                                   nullptr, nullptr)) {
            std::println(stderr, "SDL_WaitAndAcquireGPUSwapchainTexture failed: {}",
                         SDL_GetError());
            // Nothing was acquired, so cancel rather than submit.
            SDL_CancelGPUCommandBuffer(cmd);
            exit_code = EXIT_FAILURE;
            break;
        }

        if (swapchain) {
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
                exit_code = EXIT_FAILURE;
                break;
            }
            SDL_EndGPURenderPass(pass);
            SDL_SubmitGPUCommandBuffer(cmd);

            // Only a frame that actually reached the swapchain counts.
            ++presented;

            const auto now = clock::now();
            const auto frame_ms =
                std::chrono::duration<double, std::milli>(now - last).count();
            last = now;

            if (presented % 60 == 0) {
                std::println(stderr, "frame: {:.2f} ms", frame_ms);
            }

            if (opts.frames > 0 && presented >= opts.frames) {
                running = false;
            }
        } else {
            // Minimized or occluded: not an error, but no frame was presented.
            SDL_SubmitGPUCommandBuffer(cmd);
            SDL_Delay(16);
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

    return exit_code;
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