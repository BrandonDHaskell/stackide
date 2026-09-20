#version 450

// M1 textured quad: samples the checkerboard texture unchanged. Opaque, no
// blending -- see docs/buildouts/M1-textured-quad.md non-goals.
//
// SDL_GPU's SPIR-V resource binding convention for graphics shaders (see
// SDL_CreateGPUShader in SDL3/SDL_gpu.h): fragment-stage sampled textures
// use set = 2.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D tex;

void main() {
    out_color = texture(tex, in_uv);
}
