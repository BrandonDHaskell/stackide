#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace stackide {

inline constexpr int kCheckerSize = 64;
inline constexpr int kCheckerCell = 8;
inline constexpr std::array<std::uint8_t, 4> kCheckerLight = {0xE0, 0xE0, 0xE0, 0xFF};
inline constexpr std::array<std::uint8_t, 4> kCheckerDark = {0x20, 0x20, 0x20, 0xFF};

// Generated procedurally, not loaded from a file: no image-decoding
// dependency needed for a two-color grid.
std::vector<std::uint8_t> make_checkerboard(int size, int cell,
                                            std::array<std::uint8_t, 4> light,
                                            std::array<std::uint8_t, 4> dark);

struct QuadResources {
    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    SDL_GPUBuffer* vertex_buffer = nullptr;
    SDL_GPUBuffer* index_buffer = nullptr;
    SDL_GPUTexture* texture = nullptr;
    SDL_GPUSampler* sampler = nullptr;
};

void destroy_quad_resources(SDL_GPUDevice* gpu, const QuadResources& res);

// Creates the pipeline targeting color_format (the caller's actual render
// target format -- the swapchain's or the offscreen texture's, they can
// differ) plus the vertex/index buffers and checkerboard texture, uploading
// all three through one transfer buffer. Returns std::nullopt, having
// cleaned up anything already created, on any failure.
std::optional<QuadResources> create_quad_resources(SDL_GPUDevice* gpu,
                                                    SDL_GPUTextureFormat color_format);

} // namespace stackide
