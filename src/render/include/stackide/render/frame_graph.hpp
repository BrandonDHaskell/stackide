#pragma once

#include <stackide/profile/frame_metrics.hpp>
#include <stackide/render/gpu.hpp>

#include <array>
#include <optional>

namespace stackide::render
{

    // The layout constants the vertex shader draws with. Sizes are in physical pixels.
    // NOLINTBEGIN(readability-magic-numbers): these definitions are what names the numbers
    struct FrameGraphLayout
    {
        float bar_width_px = 3.0F;
        float strip_height_px = 64.0F;
        float margin_px = 8.0F;
        float strip_gap_px = 8.0F;
        float line_height_px = 1.0F;
        // Where the limit line sits, so that a bar over the limit has room to show by how much.
        float limit_position = 0.5F;
    };
    // NOLINTEND(readability-magic-numbers)

    // Bars only, since text needs the Phase 1 glyph atlas: frame time along the bottom, input to
    // paint above it, each against a line at its limit. Green or blue is within the limit, and
    // red is over it.
    class FrameGraph
    {
    public:
        // Empty on failure, having logged why. color_format is the render target's format.
        static std::optional<FrameGraph> create(const Device& device, SDL_GPUTextureFormat color_format);

        // Records into an open pass, with no uploads or allocations. Values are relative to
        // each strip's limit, oldest first, and negative where there's no sample.
        void draw(CommandBuffer& commands, RenderPass& pass, Uint32 viewport_width, Uint32 viewport_height,
                  const std::array<float, kFrameHistorySize>& frames,
                  const std::array<float, kFrameHistorySize>& inputs) const;

    private:
        explicit FrameGraph(Pipeline pipeline) noexcept : pipeline_(std::move(pipeline)) {}

        Pipeline pipeline_;
    };

} // namespace stackide::render
