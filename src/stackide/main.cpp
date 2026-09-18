//
// Created by bhaskell on 9/14/26.
//
#include "SDL3/SDL_error.h"
#include "SDL3/SDL_gpu.h"
#include "SDL3/SDL_init.h"
#include "SDL3/SDL_oldnames.h"
#include "SDL3/SDL_stdinc.h"
#include "SDL3/SDL_video.h"

#include <chrono>
#include <cstdlib>
#include <print>
#include <string_view>

namespace {

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 800;

bool has_flag(int argc, char* argv[], const std::string_view& flag) {
  for (int i = 1; i < argc; ++i) {
    if (flag == argv[i]) return true;
  }
  return false;
}

}// namespace

int main(int argc, char** argv) {
  const bool smoke = has_flag(argc, argv, "--smoke");

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::println(stderr, "SDL_Init failed: {}\n", SDL_GetError());
    return EXIT_FAILURE;
  }

  SDL_Window* window = SDL_CreateWindow("stackide", kWindowWidth, kWindowHeight,SDL_WINDOW_RESIZABLE);

  if (!window) {
    std::println(stderr, "SDL_CreateWindow failed: {}\n", SDL_GetError());
    SDL_QUIT();
    return EXIT_FAILURE;
  }

  if (SDL_ClaimWindowForGPUDevice(gpu, window)) {}
    std::println(stderr, "SDL_ClaimWindowForGPUDevice failed: {}\n", SDL_GetError());
    SDL_DestroyGPUDevice(gpu);
    SDL_DestroyWindow(window);
        SDL_Quit();
        return EXIT_FAILURE;
    }

    std::println(stderr, "gpu driver: {}", SDL_GetGPUDeviceDriver(gpu));

    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    bool running = true;
    int frames = 0;

    while (running) {
        SDL_Event event;
    while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) running = false;
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            }
        }

        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu);
        if (!cmd) {
            std::println(stderr, "AcquireGPUCommandBuffer failed: {}", SDL_GetError());
            break;
        }

        SDL_GPUTexture* swapchain = nullptr;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swapchain, nullptr, nullptr)) {
            std::println(stderr, "AcquireGPUSwapchainTexture failed: {}", SDL_GetError());
            SDL_SubmitGPUCommandBuffer(cmd);
            break;
        }

        if (swapchain) {
            SDL_GPUColorTargetInfo target{};
            target.texture = swapchain;
            target.clear_color = SDL_FColor{0.10f, 0.10f, 0.12f, 1.0f};
            target.load_op = SDL_GPU_LOADOP_CLEAR;
            target.store_op = SDL_GPU_STOREOP_STORE;

            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
            SDL_EndGPURenderPass(pass);
        }

        SDL_SubmitGPUCommandBuffer(cmd);

        const auto now = clock::now();
        const auto frame_us =
            std::chrono::duration_cast<std::chrono::microseconds>(now - last).count();
        last = now;
        ++frames;

        if (frames % 60 == 0) {
            std::println(stderr, "frame: {:.2f} ms", frame_us / 1000.0);
        }

        if (smoke && frames >= 3) running = false;
    }

    SDL_ReleaseWindowFromGPUDevice(gpu, window);
    SDL_DestroyGPUDevice(gpu);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return EXIT_SUCCESS;
}
