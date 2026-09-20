#version 450

// M1 textured quad: passthrough position/UV, scaled to letterbox the quad
// into the window's aspect ratio (see Phase 8, resize handling).
//
// SDL_GPU's SPIR-V resource binding convention for graphics shaders (see
// SDL_CreateGPUShader in SDL3/SDL_gpu.h): vertex-stage uniform buffers use
// set = 1.

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;

layout(location = 0) out vec2 out_uv;

layout(set = 1, binding = 0) uniform Scale {
    vec2 scale;
};

void main() {
    gl_Position = vec4(in_pos * scale, 0.0, 1.0);
    out_uv = in_uv;
}
