#include <stackide/modes.hpp>

#include <stackide/constants.hpp>
#include <stackide/scope_exit.hpp>

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <print>

namespace stackide {
namespace {

bool pixel_matches(const std::uint8_t* px) {
    const auto near = [](std::uint8_t got, std::uint8_t want) {
        return std::abs(static_cast<int>(got) - static_cast<int>(want)) <= kChannelTolerance;
    };
    return near(px[0], kClearR) && near(px[1], kClearG) &&
           near(px[2], kClearB) && near(px[3], kClearA);
}

} // namespace

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

} // namespace stackide
