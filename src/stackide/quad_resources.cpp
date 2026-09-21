#include <stackide/quad_resources.hpp>

#include <stackide/constants.hpp>
#include <stackide/scope_exit.hpp>
#include <stackide/shaders/quad_frag_spv.hpp>
#include <stackide/shaders/quad_vert_spv.hpp>

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstring>
#include <print>

namespace stackide {
namespace {

struct Vertex {
    float x, y;
    float u, v;
};

// Unit quad in clip space. Aspect-ratio scaling is applied via the vertex
// shader's uniform (see kIdentityScale below; real letterboxing lands with
// resize handling), not baked into these positions.
inline constexpr Vertex kQuadVertices[] = {
    {-1.0f, -1.0f, 0.0f, 1.0f},
    {1.0f, -1.0f, 1.0f, 1.0f},
    {1.0f, 1.0f, 1.0f, 0.0f},
    {-1.0f, 1.0f, 0.0f, 0.0f},
};

inline constexpr Uint16 kQuadIndices[] = {0, 1, 2, 0, 2, 3};

} // namespace

std::vector<std::uint8_t> make_checkerboard(int size, int cell,
                                            std::array<std::uint8_t, 4> light,
                                            std::array<std::uint8_t, 4> dark) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const bool is_light = ((x / cell) + (y / cell)) % 2 == 0;
            const auto& c = is_light ? light : dark;
            const auto idx = (static_cast<std::size_t>(y) * static_cast<std::size_t>(size) +
                              static_cast<std::size_t>(x)) * 4;
            pixels[idx + 0] = c[0];
            pixels[idx + 1] = c[1];
            pixels[idx + 2] = c[2];
            pixels[idx + 3] = c[3];
        }
    }
    return pixels;
}

void destroy_quad_resources(SDL_GPUDevice* gpu, const QuadResources& res) {
    if (res.pipeline) SDL_ReleaseGPUGraphicsPipeline(gpu, res.pipeline);
    if (res.sampler) SDL_ReleaseGPUSampler(gpu, res.sampler);
    if (res.texture) SDL_ReleaseGPUTexture(gpu, res.texture);
    if (res.vertex_buffer) SDL_ReleaseGPUBuffer(gpu, res.vertex_buffer);
    if (res.index_buffer) SDL_ReleaseGPUBuffer(gpu, res.index_buffer);
}

std::optional<QuadResources> create_quad_resources(SDL_GPUDevice* gpu,
                                                    SDL_GPUTextureFormat color_format) {
    SDL_GPUShaderCreateInfo vert_info{};
    vert_info.code = kQuadVertSpv.data();
    vert_info.code_size = kQuadVertSpv.size();
    vert_info.entrypoint = "main";
    vert_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    vert_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vert_info.num_uniform_buffers = 1;
    SDL_GPUShader* vert_shader = SDL_CreateGPUShader(gpu, &vert_info);
    if (!vert_shader) {
        std::println(stderr, "SDL_CreateGPUShader (vertex) failed: {}", SDL_GetError());
        return std::nullopt;
    }
    ScopeExit destroy_vert{[gpu, vert_shader] { SDL_ReleaseGPUShader(gpu, vert_shader); }};

    SDL_GPUShaderCreateInfo frag_info{};
    frag_info.code = kQuadFragSpv.data();
    frag_info.code_size = kQuadFragSpv.size();
    frag_info.entrypoint = "main";
    frag_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    frag_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    frag_info.num_samplers = 1;
    SDL_GPUShader* frag_shader = SDL_CreateGPUShader(gpu, &frag_info);
    if (!frag_shader) {
        std::println(stderr, "SDL_CreateGPUShader (fragment) failed: {}", SDL_GetError());
        return std::nullopt;
    }
    ScopeExit destroy_frag{[gpu, frag_shader] { SDL_ReleaseGPUShader(gpu, frag_shader); }};

    constexpr Uint32 kVertexBytes = sizeof(kQuadVertices);
    constexpr Uint32 kIndexBytes = sizeof(kQuadIndices);

    SDL_GPUBufferCreateInfo vb_info{};
    vb_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vb_info.size = kVertexBytes;
    SDL_GPUBuffer* vertex_buffer = SDL_CreateGPUBuffer(gpu, &vb_info);
    if (!vertex_buffer) {
        std::println(stderr, "SDL_CreateGPUBuffer (vertex) failed: {}", SDL_GetError());
        return std::nullopt;
    }
    ScopeExit destroy_vb{[gpu, vertex_buffer] { SDL_ReleaseGPUBuffer(gpu, vertex_buffer); }};

    SDL_GPUBufferCreateInfo ib_info{};
    ib_info.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    ib_info.size = kIndexBytes;
    SDL_GPUBuffer* index_buffer = SDL_CreateGPUBuffer(gpu, &ib_info);
    if (!index_buffer) {
        std::println(stderr, "SDL_CreateGPUBuffer (index) failed: {}", SDL_GetError());
        return std::nullopt;
    }
    ScopeExit destroy_ib{[gpu, index_buffer] { SDL_ReleaseGPUBuffer(gpu, index_buffer); }};

    // The checkerboard is always R8G8B8A8_UNORM_SRGB regardless of the
    // render target's format: sampling doesn't require the two to match,
    // only the pipeline's color target description has to match the real
    // render target (see color_format below).
    const std::vector<std::uint8_t> checker =
        make_checkerboard(kCheckerSize, kCheckerCell, kCheckerLight, kCheckerDark);
    const auto checker_bytes = static_cast<Uint32>(checker.size());

    SDL_GPUTextureCreateInfo tex_info{};
    tex_info.type = SDL_GPU_TEXTURETYPE_2D;
    tex_info.format = kOffscreenColorFormat;
    tex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tex_info.width = static_cast<Uint32>(kCheckerSize);
    tex_info.height = static_cast<Uint32>(kCheckerSize);
    tex_info.layer_count_or_depth = 1;
    tex_info.num_levels = 1;
    tex_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(gpu, &tex_info);
    if (!texture) {
        std::println(stderr, "SDL_CreateGPUTexture (checkerboard) failed: {}", SDL_GetError());
        return std::nullopt;
    }
    ScopeExit destroy_tex{[gpu, texture] { SDL_ReleaseGPUTexture(gpu, texture); }};

    const Uint32 upload_size = kVertexBytes + kIndexBytes + checker_bytes;
    SDL_GPUTransferBufferCreateInfo tb_info{};
    tb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tb_info.size = upload_size;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(gpu, &tb_info);
    if (!transfer) {
        std::println(stderr, "SDL_CreateGPUTransferBuffer (upload) failed: {}", SDL_GetError());
        return std::nullopt;
    }
    ScopeExit destroy_transfer{[gpu, transfer] { SDL_ReleaseGPUTransferBuffer(gpu, transfer); }};

    auto* mapped = static_cast<std::uint8_t*>(SDL_MapGPUTransferBuffer(gpu, transfer, false));
    if (!mapped) {
        std::println(stderr, "SDL_MapGPUTransferBuffer (upload) failed: {}", SDL_GetError());
        return std::nullopt;
    }
    std::memcpy(mapped, kQuadVertices, kVertexBytes);
    std::memcpy(mapped + kVertexBytes, kQuadIndices, kIndexBytes);
    std::memcpy(mapped + kVertexBytes + kIndexBytes, checker.data(), checker_bytes);
    SDL_UnmapGPUTransferBuffer(gpu, transfer);

    SDL_GPUCommandBuffer* upload_cmd = SDL_AcquireGPUCommandBuffer(gpu);
    if (!upload_cmd) {
        std::println(stderr, "SDL_AcquireGPUCommandBuffer (upload) failed: {}", SDL_GetError());
        return std::nullopt;
    }

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(upload_cmd);
    if (!copy) {
        std::println(stderr, "SDL_BeginGPUCopyPass (upload) failed: {}", SDL_GetError());
        SDL_CancelGPUCommandBuffer(upload_cmd);
        return std::nullopt;
    }

    const SDL_GPUTransferBufferLocation vb_src{transfer, 0};
    const SDL_GPUBufferRegion vb_dst{vertex_buffer, 0, kVertexBytes};
    SDL_UploadToGPUBuffer(copy, &vb_src, &vb_dst, false);

    const SDL_GPUTransferBufferLocation ib_src{transfer, kVertexBytes};
    const SDL_GPUBufferRegion ib_dst{index_buffer, 0, kIndexBytes};
    SDL_UploadToGPUBuffer(copy, &ib_src, &ib_dst, false);

    SDL_GPUTextureTransferInfo tex_src{};
    tex_src.transfer_buffer = transfer;
    tex_src.offset = kVertexBytes + kIndexBytes;
    tex_src.pixels_per_row = static_cast<Uint32>(kCheckerSize);
    tex_src.rows_per_layer = static_cast<Uint32>(kCheckerSize);

    SDL_GPUTextureRegion tex_dst{};
    tex_dst.texture = texture;
    tex_dst.w = static_cast<Uint32>(kCheckerSize);
    tex_dst.h = static_cast<Uint32>(kCheckerSize);
    tex_dst.d = 1;
    SDL_UploadToGPUTexture(copy, &tex_src, &tex_dst, false);

    SDL_EndGPUCopyPass(copy);

    if (!SDL_SubmitGPUCommandBuffer(upload_cmd)) {
        std::println(stderr, "SDL_SubmitGPUCommandBuffer (upload) failed: {}", SDL_GetError());
        return std::nullopt;
    }

    SDL_GPUSamplerCreateInfo sampler_info{};
    sampler_info.min_filter = SDL_GPU_FILTER_NEAREST;
    sampler_info.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    // Nearest filtering is deliberate: the offscreen pixel assertions (added
    // alongside this) check exact bytes on each side of a checker boundary,
    // which linear filtering would blend across.
    SDL_GPUSampler* sampler = SDL_CreateGPUSampler(gpu, &sampler_info);
    if (!sampler) {
        std::println(stderr, "SDL_CreateGPUSampler failed: {}", SDL_GetError());
        return std::nullopt;
    }
    ScopeExit destroy_sampler{[gpu, sampler] { SDL_ReleaseGPUSampler(gpu, sampler); }};

    const SDL_GPUVertexBufferDescription vb_desc{
        .slot = 0,
        .pitch = sizeof(Vertex),
        .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX,
        .instance_step_rate = 0,
    };
    const SDL_GPUVertexAttribute attrs[2] = {
        {.location = 0,
         .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
         .offset = offsetof(Vertex, x)},
        {.location = 1,
         .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
         .offset = offsetof(Vertex, u)},
    };

    SDL_GPUColorTargetDescription color_target{};
    color_target.format = color_format;
    color_target.blend_state.enable_blend = false;

    SDL_GPUGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.vertex_shader = vert_shader;
    pipeline_info.fragment_shader = frag_shader;
    pipeline_info.vertex_input_state.vertex_buffer_descriptions = &vb_desc;
    pipeline_info.vertex_input_state.num_vertex_buffers = 1;
    pipeline_info.vertex_input_state.vertex_attributes = attrs;
    pipeline_info.vertex_input_state.num_vertex_attributes = 2;
    pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipeline_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pipeline_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pipeline_info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    pipeline_info.depth_stencil_state.enable_depth_test = false;
    pipeline_info.target_info.color_target_descriptions = &color_target;
    pipeline_info.target_info.num_color_targets = 1;
    pipeline_info.target_info.has_depth_stencil_target = false;

    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(gpu, &pipeline_info);
    if (!pipeline) {
        std::println(stderr, "SDL_CreateGPUGraphicsPipeline failed: {}", SDL_GetError());
        return std::nullopt;
    }

    destroy_sampler.release();
    destroy_tex.release();
    destroy_ib.release();
    destroy_vb.release();

    return QuadResources{
        .pipeline = pipeline,
        .vertex_buffer = vertex_buffer,
        .index_buffer = index_buffer,
        .texture = texture,
        .sampler = sampler,
    };
}

} // namespace stackide
