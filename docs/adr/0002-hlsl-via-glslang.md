# ADR-0002: HLSL shaders compiled with glslang

Status: accepted
Date: 2026-11-05
Issue: #7

## Context

Issue #7 specified HLSL compiled by SDL_shadercross. SDL_shadercross has no releases
or tags, so it can't be pinned by release archive and SHA256, and its HLSL path needs
DXC and SPIRV submodules. M1 used GLSL with glslang, which is already in the toolchain.

## Decision

Write shaders in HLSL and compile them to SPIR-V with glslang. Embed the bytes in the
binary, as before. Metal shaders wait until Metal rendering is real work.

## How shaders must declare resources

glslang's output has to match the layout SDL_GPU creates. See `SDL_CreateGPUShader` in
`SDL3/SDL_gpu.h` for the sets, which are not the usual HLSL register spaces.

| Stage | Set | Holds |
|---|---|---|
| Vertex | 0 | Sampled textures |
| Vertex | 1 | Uniform buffers |
| Fragment | 2 | Sampled textures |
| Fragment | 3 | Uniform buffers |

Use explicit `[[vk::binding(binding, set)]]` and `[[vk::location(n)]]`, never
glslang's automatic assignment.

**A sampled texture and its sampler must share one binding:**

    [[vk::binding(0, 2)]] Texture2D checker_texture;
    [[vk::binding(0, 2)]] SamplerState checker_sampler;

SDL_GPU only creates combined image-sampler descriptors. Giving the sampler its own
binding produces a layout that doesn't match. The other routes tried with glslang
don't work: `sampler2D` emits a sampler with no image, and it doesn't recognize DXC's
`[[vk::combinedImageSampler]]`.

## Verification

- `spirv-val` accepts the output for Vulkan 1.2.
- `--offscreen` pixel checks pass on the NVIDIA driver and on lavapipe.
- Under the Khronos validation layers (1.4.309, on lavapipe), these shaders report no
  errors, while a sampler at its own binding reports
  `VUID-VkGraphicsPipelineCreateInfo-layout-07988`. That shader still rendered correct
  pixels on lavapipe, which is why pixels alone can't check this.

SDL installs no debug messenger, so a `--gpu-debug` run sends the layer's output to a
file and fails on any error in it. `gpu.validation_gate` proves that with a bad shader,
and `smoke.offscreen_validated` runs the real ones. The windowed path isn't validated
yet: CI has no display, and the development machine has no layers installed.

## Consequences

- The HLSL subset glslang supports is narrower than DXC's, and it trails newer shader
  features. A shader that needs more would reopen this decision.
- Metal needs MSL later, most likely by translating this SPIR-V.
