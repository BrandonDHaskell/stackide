#pragma once

#include <stackide/render/gpu.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace stackide
{

    inline constexpr int kCheckerSize = 64;
    inline constexpr int kCheckerCell = 8;
    // Each channel differs, so that the offscreen pixel checks catch swapped channels.
    inline constexpr std::array<std::uint8_t, 4> kCheckerLight = {0xE0, 0xC0, 0xA0, 0xFF};
    inline constexpr std::array<std::uint8_t, 4> kCheckerDark = {0x20, 0x30, 0x40, 0xFF};
    inline constexpr Uint32 kQuadIndexCount = 6;

    // Generated procedurally, not loaded from a file: no image-decoding
    // dependency needed for a two-color grid.
    std::vector<std::uint8_t> make_checkerboard(int size, int cell, std::array<std::uint8_t, 4> light,
                                                std::array<std::uint8_t, 4> dark);

    struct QuadResources
    {
        render::Pipeline pipeline;
        render::Buffer vertex_buffer;
        render::Buffer index_buffer;
        render::Texture texture;
        render::Sampler sampler;
    };

    // Creates the pipeline targeting color_format (the caller's actual render target's
    // format, since the swapchain's and the offscreen texture's can differ) and uploads
    // the quad and its checkerboard texture. Empty on failure, having logged why.
    std::optional<QuadResources> create_quad_resources(const render::Device& device, SDL_GPUTextureFormat color_format);

    // Binds everything and draws. scale shrinks the quad along each axis, which is how a
    // window keeps it square.
    void draw_quad(render::CommandBuffer& commands, render::RenderPass& pass, const QuadResources& quad,
                   std::array<float, 2> scale);

} // namespace stackide
