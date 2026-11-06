#include <stackide/render/frame_graph.hpp>

#include <stackide/shaders/frame_graph_frag_spv.hpp>
#include <stackide/shaders/frame_graph_vert_spv.hpp>

#include <cstddef>
#include <utility>

namespace stackide::render
{
    namespace
    {

        constexpr std::size_t kFloatsPerRow = 4;
        constexpr std::size_t kRows = 2 * kFrameHistorySize / kFloatsPerRow;
        constexpr Uint32 kVerticesPerRectangle = 6;
        constexpr Uint32 kLimitLines = 2;
        constexpr Uint32 kVertexCount = ((2 * kFrameHistorySize) + kLimitLines) * kVerticesPerRectangle;

        // Must match the cbuffer in shaders/frame_graph.vert.hlsl.
        struct Uniforms
        {
            std::array<float, 2> viewport_px{};
            FrameGraphLayout layout;
            std::array<std::array<float, kFloatsPerRow>, kRows> values{};
        };
        static_assert(sizeof(Uniforms) ==
                      sizeof(std::array<float, 2>) + sizeof(FrameGraphLayout) +
                          (2 * kFrameHistorySize * sizeof(float)));

    } // namespace

    std::optional<FrameGraph> FrameGraph::create(const Device& device, SDL_GPUTextureFormat color_format)
    {
        SDL_GPUShaderCreateInfo vert_info{};
        vert_info.code = kFrameGraphVertSpv.data();
        vert_info.code_size = kFrameGraphVertSpv.size();
        vert_info.entrypoint = "main";
        vert_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        vert_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        vert_info.num_uniform_buffers = 1;
        const Shader vert_shader = device.create_shader(vert_info, "frame graph vertex shader");

        SDL_GPUShaderCreateInfo frag_info{};
        frag_info.code = kFrameGraphFragSpv.data();
        frag_info.code_size = kFrameGraphFragSpv.size();
        frag_info.entrypoint = "main";
        frag_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        frag_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        const Shader frag_shader = device.create_shader(frag_info, "frame graph fragment shader");
        if (!vert_shader || !frag_shader)
        {
            return std::nullopt;
        }

        SDL_GPUColorTargetDescription color_target{};
        color_target.format = color_format;

        SDL_GPUGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.vertex_shader = vert_shader.get();
        pipeline_info.fragment_shader = frag_shader.get();
        pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pipeline_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pipeline_info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        pipeline_info.target_info.color_target_descriptions = &color_target;
        pipeline_info.target_info.num_color_targets = 1;

        Pipeline pipeline = device.create_pipeline(pipeline_info, "frame graph pipeline");
        if (!pipeline)
        {
            return std::nullopt;
        }
        return FrameGraph{std::move(pipeline)};
    }

    void FrameGraph::draw(CommandBuffer& commands, RenderPass& pass, Uint32 viewport_width, Uint32 viewport_height,
                          const std::array<float, kFrameHistorySize>& frames,
                          const std::array<float, kFrameHistorySize>& inputs) const
    {
        Uniforms uniforms{};
        uniforms.viewport_px = {static_cast<float>(viewport_width), static_cast<float>(viewport_height)};
        for (std::size_t i = 0; i < kFrameHistorySize; ++i)
        {
            uniforms.values.at(i / kFloatsPerRow).at(i % kFloatsPerRow) = frames.at(i);
            const std::size_t input_slot = kFrameHistorySize + i;
            uniforms.values.at(input_slot / kFloatsPerRow).at(input_slot % kFloatsPerRow) = inputs.at(i);
        }

        pass.bind_pipeline(pipeline_);
        commands.push_vertex_uniform(&uniforms, sizeof(uniforms));
        pass.draw(kVertexCount);
    }

} // namespace stackide::render
