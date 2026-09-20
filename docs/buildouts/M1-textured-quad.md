# Milestone 1: Textured Quad

Status: open
Opened: 2026-09-20
Owner: Brandon

## Goal

Render a single textured quad through a real SDL_GPU graphics pipeline, with
frame timing instrumented and verified at both refresh rates the development
display supports.

## Why this one

Everything the editor needs downstream is in this milestone: shader compilation
and embedding, a graphics pipeline object, vertex buffers, transfer buffers,
samplers, and texture upload. A glyph atlas is a textured quad with different
UVs. Getting this correct once means the text pipeline is a content problem
rather than an infrastructure problem.

## Definition of done

Each item is verifiable by a command. An item with no way to check it is not
done, it is asserted.

1. **Shaders compile at build time.** GLSL sources under `shaders/` compile to
   SPIR-V via a CMake custom command and are embedded in the binary. No shader
   files are read at runtime and no shader source ships with the binary.
2. **Quad renders.** A single quad samples a generated checkerboard texture
   uploaded through a transfer buffer. Correct orientation, correct UVs,
   correct aspect at the default window size.
3. **Offscreen mode verifies the quad, not a clear.** `--offscreen` renders the
   quad and asserts pixel values at four known UV positions, including at least
   one on each side of a checker boundary. The existing clear assertion stays.
4. **Present mode is selectable.** `--present-mode vsync|immediate`. Immediate
   is how frame cost gets measured; vsync is how cadence gets verified.
5. **Frame timing is reported.** On exit, the binary prints p50, p99, and max
   CPU frame time over the run, plus the count of frames presented.
6. **Frame cost gate.** In immediate mode at 1280x800, p99 CPU frame time is
   under 2.0 ms over 1000 frames. One quad costing more than that indicates a
   structural problem, not a tuning problem.
7. **Cadence gate at both refresh rates.** In vsync mode, zero dropped frames
   over 600 presented frames at 60 Hz and at 100 Hz. Dropped means a frame
   interval exceeding 1.5 refresh intervals.
8. **Resize is handled.** Window resize does not crash, does not leak, and the
   quad keeps its aspect ratio. Verified by a manual resize and by a
   `--frames`-bounded run after a programmatic resize.
9. **Clean build.** Zero warnings with `-Werror` on the `gcc`, `clang`,
   `gcc-release`, and `clang-release` presets.
10. **Clean static analysis.** `clang-tidy` reports nothing under the check set
    in `.clangd`.
11. **All tests green.** `ctest --preset gcc` and `--preset clang` pass locally;
    `ctest --preset ci-gcc` and `--preset ci-clang` pass in the toolchain
    container.

## Non-goals

Explicitly out of scope. Work matching any of these is deferred, not
opportunistically included.

- Text, fonts, glyph atlases, shaping
- More than one draw call, batching, instancing
- A scene graph, a renderer abstraction layer, or any interface with one
  implementation
- Editor concepts: buffers, cursors, documents
- Agent concepts of any kind
- Plugin or capability-provider interfaces
- Configuration files or settings persistence
- macOS support beyond keeping Metal viable as a later target

## Decisions this milestone forces

These were open and are now settled. Each is binding for the rest of this
milestone's implementation.

1. **Shader compilation toolchain.** Settled 2026-09-19: `glslang-tools` via
   `apt`, in both the CI toolchain container and the host dev machine. Not
   vendored under `/opt` like GCC/LLVM/CMake/Ninja — it is a leaf build tool,
   apt-managed is proportionate. Exact version is recorded from
   `dpkg -s glslang-tools` at build time, not pinned in advance.
2. **Cross-compilation for Metal.** Settled 2026-09-19: deferred. SPIR-V only
   for this milestone, consistent with the non-goal above (macOS support
   beyond keeping Metal viable later). No SDL_shadercross dependency is added.
3. **Swapchain colorspace.** Settled 2026-09-19: `SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR`.
   The swapchain, the offscreen render target, and the checkerboard texture
   all use the matching `_SRGB` `SDL_GPUTextureFormat`. Reason: the eventual
   glyph atlas needs linear-space alpha blending for antialiased edges, and
   this should be load-bearing now rather than retrofitted once text
   rendering exists.
4. **Shader embedding mechanism.** Settled 2026-09-19: a CMake-generated
   byte-array header now. Revisit `#embed` once both GCC 16 and Clang support
   it in C++ mode.

## Measurement definitions

The criteria doc gives a frame budget; these define how it is measured so the
numbers mean the same thing every time.

| Refresh | Interval | Budget |
|---|---|---|
| 60 Hz | 16.67 ms | 16.0 ms working |
| 100 Hz | 10.00 ms | 10.0 ms |

The development display runs both. The 100 Hz budget is the binding one and is
what the gates above are set against.

**CPU frame time** is measured from the top of the loop iteration to the return
of `SDL_SubmitGPUCommandBuffer`. It excludes the vsync wait, which is why frame
cost is measured in immediate mode.

**Input to paint** is measured from the SDL event timestamp to the return of
`SDL_SubmitGPUCommandBuffer` for the frame that reflects it. It deliberately
excludes the wait for the next vblank: where in the refresh interval the input
landed is not a property of the code. Under vsync the displayed latency is this
value plus up to one refresh interval.

## Exit criteria

```sh
cmake --preset gcc && cmake --build --preset gcc && ctest --preset gcc
cmake --preset clang && cmake --build --preset clang && ctest --preset clang
./build/gcc/stackide --present-mode immediate --frames 1000   # check p99
./build/gcc/stackide --present-mode vsync --frames 600        # check drops
```

Milestone closes when all eleven items pass and the four decisions are recorded
in this file.