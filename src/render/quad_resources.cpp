#include <stackide/render/quad_resources.hpp>

#include <stackide/base/log.hpp>
#include <stackide/render/render_constants.hpp>
#include <stackide/render/upload_ring.hpp>
#include <stackide/shaders/quad_frag_spv.hpp>
#include <stackide/shaders/quad_vert_spv.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace stackide
{
    namespace
    {

        struct Vertex
        {
            float x, y;
            float u, v;
        };

        // Unit quad in clip space. Aspect-ratio scaling is applied via the vertex
        // shader's uniform, not baked into these positions.
        inline constexpr std::array<Vertex, 4> kQuadVertices = {
            {
                {.x = -1.0F, .y = -1.0F, .u = 0.0F, .v = 1.0F},
                {.x = 1.0F, .y = -1.0F, .u = 1.0F, .v = 1.0F},
                {.x = 1.0F, .y = 1.0F, .u = 1.0F, .v = 0.0F},
                {.x = -1.0F, .y = 1.0F, .u = 0.0F, .v = 0.0F},
            },
        };

        inline constexpr std::array<Uint16, kQuadIndexCount> kQuadIndices = {0, 1, 2, 0, 2, 3};

    } // namespace

    // `size` and `cell` are pixel counts on unrelated scales (texture dimension
    // vs. checker-cell dimension) and are never used positionally elsewhere, so
    // a swap would be an obvious value-out-of-range bug at the call site rather
    // than a silent one.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    std::vector<std::uint8_t> make_checkerboard(int size, int cell, std::array<std::uint8_t, 4> light,
                                                std::array<std::uint8_t, 4> dark)
    {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4);
        for (int y = 0; y < size; ++y)
        {
            for (int x = 0; x < size; ++x)
            {
                const bool is_light = ((x / cell) + (y / cell)) % 2 == 0;
                const auto& c = is_light ? light : dark;
                const auto idx =
                    ((static_cast<std::size_t>(y) * static_cast<std::size_t>(size)) + static_cast<std::size_t>(x)) * 4;
                pixels.at(idx + 0) = c.at(0);
                pixels.at(idx + 1) = c.at(1);
                pixels.at(idx + 2) = c.at(2);
                pixels.at(idx + 3) = c.at(3);
            }
        }
        return pixels;
    }

    std::optional<QuadResources> create_quad_resources(const render::Device& device, SDL_GPUTextureFormat color_format)
    {
        SDL_GPUShaderCreateInfo vert_info{};
        vert_info.code = kQuadVertSpv.data();
        vert_info.code_size = kQuadVertSpv.size();
        vert_info.entrypoint = "main";
        vert_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        vert_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        vert_info.num_uniform_buffers = 1;
        const render::Shader vert_shader = device.create_shader(vert_info, "vertex shader");

        SDL_GPUShaderCreateInfo frag_info{};
        frag_info.code = kQuadFragSpv.data();
        frag_info.code_size = kQuadFragSpv.size();
        frag_info.entrypoint = "main";
        frag_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        frag_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        frag_info.num_samplers = 1;
        const render::Shader frag_shader = device.create_shader(frag_info, "fragment shader");
        if (!vert_shader || !frag_shader)
        {
            return std::nullopt;
        }

        const auto vertex_bytes = std::as_bytes(std::span{kQuadVertices});
        const auto index_bytes = std::as_bytes(std::span{kQuadIndices});

        SDL_GPUBufferCreateInfo vb_info{};
        vb_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        vb_info.size = static_cast<Uint32>(vertex_bytes.size());
        render::Buffer vertex_buffer = device.create_buffer(vb_info, "vertex buffer");

        SDL_GPUBufferCreateInfo ib_info{};
        ib_info.usage = SDL_GPU_BUFFERUSAGE_INDEX;
        ib_info.size = static_cast<Uint32>(index_bytes.size());
        render::Buffer index_buffer = device.create_buffer(ib_info, "index buffer");

        // The checkerboard is always R8G8B8A8_UNORM_SRGB regardless of the
        // render target's format: sampling doesn't require the two to match,
        // only the pipeline's color target description has to match the real
        // render target (see color_format below).
        const std::vector<std::uint8_t> checker =
            make_checkerboard(kCheckerSize, kCheckerCell, kCheckerLight, kCheckerDark);
        const auto checker_bytes = std::as_bytes(std::span{checker});

        SDL_GPUTextureCreateInfo tex_info{};
        tex_info.type = SDL_GPU_TEXTURETYPE_2D;
        tex_info.format = kOffscreenColorFormat;
        tex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        tex_info.width = static_cast<Uint32>(kCheckerSize);
        tex_info.height = static_cast<Uint32>(kCheckerSize);
        tex_info.layer_count_or_depth = 1;
        tex_info.num_levels = 1;
        tex_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        render::Texture texture = device.create_texture(tex_info, "checkerboard texture");

        SDL_GPUSamplerCreateInfo sampler_info{};
        sampler_info.min_filter = SDL_GPU_FILTER_NEAREST;
        sampler_info.mag_filter = SDL_GPU_FILTER_NEAREST;
        sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        // Nearest filtering is deliberate: the offscreen pixel assertions check exact
        // bytes on each side of a checker boundary, which linear filtering would blend.
        render::Sampler sampler = device.create_sampler(sampler_info, "sampler");
        if (!vertex_buffer || !index_buffer || !texture || !sampler)
        {
            return std::nullopt;
        }

        const auto upload_size = static_cast<Uint32>(vertex_bytes.size() + index_bytes.size() + checker_bytes.size());
        std::optional<render::UploadRing> ring = render::UploadRing::create(device, upload_size);
        if (!ring || !ring->begin())
        {
            return std::nullopt;
        }
        const auto vertex_source = ring->write(vertex_bytes);
        const auto index_source = ring->write(index_bytes);
        const auto checker_source = ring->write(checker_bytes);
        ring->end();
        if (!vertex_source || !index_source || !checker_source)
        {
            return std::nullopt;
        }

        std::optional<render::CommandBuffer> commands = render::CommandBuffer::acquire(device);
        if (!commands)
        {
            return std::nullopt;
        }
        {
            std::optional<render::CopyPass> copy = commands->begin_copy_pass();
            if (!copy)
            {
                return std::nullopt;
            }
            copy->upload(*vertex_source, vertex_buffer, static_cast<Uint32>(vertex_bytes.size()));
            copy->upload(*index_source, index_buffer, static_cast<Uint32>(index_bytes.size()));

            SDL_GPUTextureTransferInfo tex_source{};
            tex_source.transfer_buffer = checker_source->transfer_buffer;
            tex_source.offset = checker_source->offset;
            tex_source.pixels_per_row = static_cast<Uint32>(kCheckerSize);
            tex_source.rows_per_layer = static_cast<Uint32>(kCheckerSize);

            SDL_GPUTextureRegion tex_destination{};
            tex_destination.texture = texture.get();
            tex_destination.w = static_cast<Uint32>(kCheckerSize);
            tex_destination.h = static_cast<Uint32>(kCheckerSize);
            tex_destination.d = 1;
            copy->upload(tex_source, tex_destination);
        }
        if (!commands->submit())
        {
            return std::nullopt;
        }

        const SDL_GPUVertexBufferDescription vb_desc{
            .slot = 0,
            .pitch = sizeof(Vertex),
            .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX,
            .instance_step_rate = 0,
        };
        const std::array<SDL_GPUVertexAttribute, 2> attrs = {
            {
                {
                    .location = 0,
                    .buffer_slot = 0,
                    .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
                    .offset = offsetof(Vertex, x),
                },
                {
                    .location = 1,
                    .buffer_slot = 0,
                    .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
                    .offset = offsetof(Vertex, u),
                },
            },
        };

        SDL_GPUColorTargetDescription color_target{};
        color_target.format = color_format;
        color_target.blend_state.enable_blend = false;

        SDL_GPUGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.vertex_shader = vert_shader.get();
        pipeline_info.fragment_shader = frag_shader.get();
        pipeline_info.vertex_input_state.vertex_buffer_descriptions = &vb_desc;
        pipeline_info.vertex_input_state.num_vertex_buffers = 1;
        pipeline_info.vertex_input_state.vertex_attributes = attrs.data();
        pipeline_info.vertex_input_state.num_vertex_attributes = 2;
        pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pipeline_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pipeline_info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        pipeline_info.depth_stencil_state.enable_depth_test = false;
        pipeline_info.target_info.color_target_descriptions = &color_target;
        pipeline_info.target_info.num_color_targets = 1;
        pipeline_info.target_info.has_depth_stencil_target = false;

        render::Pipeline pipeline = device.create_pipeline(pipeline_info, "graphics pipeline");
        if (!pipeline)
        {
            return std::nullopt;
        }

        return QuadResources{
            .pipeline = std::move(pipeline),
            .vertex_buffer = std::move(vertex_buffer),
            .index_buffer = std::move(index_buffer),
            .texture = std::move(texture),
            .sampler = std::move(sampler),
        };
    }

    void draw_quad(render::CommandBuffer& commands, render::RenderPass& pass, const QuadResources& quad,
                   std::array<float, 2> scale)
    {
        pass.bind_pipeline(quad.pipeline);
        pass.bind_vertex_buffer(quad.vertex_buffer);
        pass.bind_index_buffer_16(quad.index_buffer);
        pass.bind_fragment_texture(quad.texture, quad.sampler);
        commands.push_vertex_uniform(scale.data(), sizeof(scale));
        pass.draw_indexed(kQuadIndexCount);
    }

} // namespace stackide
