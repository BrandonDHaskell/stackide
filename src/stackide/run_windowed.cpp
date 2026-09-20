#include <stackide/modes.hpp>

#include <stackide/constants.hpp>
#include <stackide/quad_resources.hpp>
#include <stackide/scope_exit.hpp>

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdlib>
#include <optional>
#include <print>

namespace stackide {

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
    const SDL_GPUTextureFormat swapchain_format = SDL_GetGPUSwapchainTextureFormat(gpu, window);
    std::println(stderr, "swapchain format: {}", static_cast<int>(swapchain_format));

    std::optional<QuadResources> quad = create_quad_resources(gpu, swapchain_format);
    if (!quad) {
        return opts.allow_skip ? kExitSkip : EXIT_FAILURE;
    }
    ScopeExit destroy_quad{[gpu, res = *quad] { destroy_quad_resources(gpu, res); }};

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

} // namespace stackide
