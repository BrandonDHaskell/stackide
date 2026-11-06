#include <stackide/platform/video.hpp>
#include <stackide/render/gpu.hpp>
#include <stackide/shaders/bad_binding_frag_spv.hpp>
#include <stackide/shaders/quad_frag_spv.hpp>
#include <stackide/shaders/quad_vert_spv.hpp>

#include <catch2/catch_test_macros.hpp>

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <print>
#include <span>

using namespace stackide;

namespace
{

    enum class Outcome : std::uint8_t
    {
        NoDevice,
        NoLayers,
        Clean,
        Failed,
    };

    // A debug device that builds one pipeline, returning what --gpu-debug would conclude.
    Outcome build_pipeline(std::span<const unsigned char> fragment_code)
    {
        Options options;
        options.gpu_debug = true;

        int result = EXIT_SUCCESS;
        {
            const auto device = render::Device::create(true);
            if (!device)
            {
                return Outcome::NoDevice;
            }

            SDL_GPUShaderCreateInfo vert_info{};
            vert_info.code = kQuadVertSpv.data();
            vert_info.code_size = kQuadVertSpv.size();
            vert_info.entrypoint = "main";
            vert_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
            vert_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
            vert_info.num_uniform_buffers = 1;
            const render::Shader vertex = device->create_shader(vert_info, "vertex shader");

            SDL_GPUShaderCreateInfo frag_info{};
            frag_info.code = fragment_code.data();
            frag_info.code_size = fragment_code.size();
            frag_info.entrypoint = "main";
            frag_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
            frag_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
            frag_info.num_samplers = 1;
            const render::Shader fragment = device->create_shader(frag_info, "fragment shader");
            REQUIRE(vertex);
            REQUIRE(fragment);

            constexpr std::array<SDL_GPUVertexAttribute, 2> kAttributes = {{
                {.location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, .offset = 0},
                {.location = 1, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, .offset = 8},
            }};
            constexpr SDL_GPUVertexBufferDescription kBuffer{
                .slot = 0, .pitch = 16, .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX, .instance_step_rate = 0};

            SDL_GPUColorTargetDescription color_target{};
            color_target.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;

            SDL_GPUGraphicsPipelineCreateInfo info{};
            info.vertex_shader = vertex.get();
            info.fragment_shader = fragment.get();
            info.vertex_input_state.vertex_buffer_descriptions = &kBuffer;
            info.vertex_input_state.num_vertex_buffers = 1;
            info.vertex_input_state.vertex_attributes = kAttributes.data();
            info.vertex_input_state.num_vertex_attributes = 2;
            info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            info.target_info.color_target_descriptions = &color_target;
            info.target_info.num_color_targets = 1;
            const render::Pipeline pipeline = device->create_pipeline(info, "pipeline");
        }

        if (platform::gpu_validation_missing())
        {
            return Outcome::NoLayers;
        }
        result = render::apply_gpu_debug_result(options, result);
        return result == EXIT_SUCCESS ? Outcome::Clean : Outcome::Failed;
    }

} // namespace

TEST_CASE("a debug run fails on a validation error, and only on one")
{
    const auto video = platform::Video::start(platform::VideoBackend::Offscreen);
    REQUIRE(video);

    const Outcome good = build_pipeline(kQuadFragSpv);
    if (good == Outcome::NoDevice || good == Outcome::NoLayers)
    {
        std::println("skipped: validation layers not installed");
        if (std::getenv("STACKIDE_REQUIRE_GPU") != nullptr)
        {
            FAIL("no validation layers, and STACKIDE_REQUIRE_GPU is set");
        }
        return;
    }

    CHECK(good == Outcome::Clean);
    CHECK(build_pipeline(kBadBindingFragSpv) == Outcome::Failed);
    CHECK(build_pipeline(kQuadFragSpv) == Outcome::Clean);
}
