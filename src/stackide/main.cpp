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

#include <stackide/shaders/quad_frag_spv.hpp>
#include <stackide/shaders/quad_vert_spv.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <print>
#include <string_view>
#include <utility>
#include <vector>

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
// Quad resources: pipeline, buffers, texture, sampler. Shared by --offscreen
// and windowed rendering -- both draw the same quad sampling the same
// generated checkerboard texture.
// ---------------------------------------------------------------------------

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

inline constexpr int kCheckerSize = 64;
inline constexpr int kCheckerCell = 8;
inline constexpr std::array<std::uint8_t, 4> kCheckerLight = {0xE0, 0xE0, 0xE0, 0xFF};
inline constexpr std::array<std::uint8_t, 4> kCheckerDark = {0x20, 0x20, 0x20, 0xFF};

// Generated procedurally, not loaded from a file: no image-decoding
// dependency needed for a two-color grid.
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

struct QuadResources {
    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    SDL_GPUBuffer* vertex_buffer = nullptr;
    SDL_GPUBuffer* index_buffer = nullptr;
    SDL_GPUTexture* texture = nullptr;
    SDL_GPUSampler* sampler = nullptr;
};

void destroy_quad_resources(SDL_GPUDevice* gpu, const QuadResources& res) {
    if (res.pipeline) SDL_ReleaseGPUGraphicsPipeline(gpu, res.pipeline);
    if (res.sampler) SDL_ReleaseGPUSampler(gpu, res.sampler);
    if (res.texture) SDL_ReleaseGPUTexture(gpu, res.texture);
    if (res.vertex_buffer) SDL_ReleaseGPUBuffer(gpu, res.vertex_buffer);
    if (res.index_buffer) SDL_ReleaseGPUBuffer(gpu, res.index_buffer);
}

// Creates the pipeline targeting color_format (the caller's actual render
// target format -- the swapchain's or the offscreen texture's, they can
// differ) plus the vertex/index buffers and checkerboard texture, uploading
// all three through one transfer buffer. Returns std::nullopt, having
// cleaned up anything already created, on any failure.
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