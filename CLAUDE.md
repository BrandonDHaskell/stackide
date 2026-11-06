# STACK

C++26 IDE with agents as a first-class subsystem. Long-horizon personal project.

## Dependencies
All third-party dependencies are declared in `cmake/Dependencies.cmake`, pinned
by release archive and SHA256. Do not add a dependency, vendor code, or call
FetchContent or find_package anywhere else. If a task appears to need a new
dependency, stop and ask.

## Permissions
Tool permissions are in `.claude/settings.json`. Denied commands are denied for
a reason; do not work around them with a shell construct that evades the rule.
If a task genuinely needs a denied capability, say so and stop.

## Build

Never invoke compilers directly. Always use presets.

    cmake --preset gcc && cmake --build --preset gcc      # primary, local GUI
    cmake --preset clang && cmake --build --preset clang  # secondary, ABI check
    ctest --preset gcc

`ci-gcc` and `ci-clang` are test presets only. They use the `gcc` and `clang`
builds, exclude display tests, and treat a GPU skip as a failure.

Sanitizers: `gcc-asan`, `clang-asan`, `gcc-tsan` and `clang-tsan` are configure and
build presets, tested with `ctest --preset ci-<name>`. TSan runs unit tests only.

Lint: `cmake --build --preset clang --target format-check tidy`.

Verify in the container before pushing. This builds in a copy inside the
container, because a CMake cache can't be shared between host and container paths:

    IMAGE=$(sed -n 's/.*container: //p' .github/workflows/ci.yml)
    docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/w:ro "$IMAGE" bash -c \
      'mkdir /tmp/w && tar -C /w --exclude=./build --exclude=./.git -cf - . | tar -xf - -C /tmp/w && cd /tmp/w && cmake --preset gcc && cmake --build --preset gcc && ctest --preset ci-gcc'

## Toolchain

All under /opt via indirection symlinks. Do not apt-install compilers.

- /opt/gcc -> GCC 16.2, primary, libstdc++, rpath /opt/gcc/lib64
- /opt/llvm -> LLVM 23.1.1, secondary, libc++ + compiler-rt + libunwind
- /opt/cmake -> 4.4.3, /opt/ninja -> 1.13.1

C++26, `CMAKE_CXX_EXTENSIONS OFF`. Headers, not `import std` (deferred deliberately).

## Hardware and environment
Target hardware, supported platforms, and all derived constants are defined in
`docs/hardware-environment.md`. That document is authoritative. Do not hardcode
tier values, pool sizes, or frame budgets; derive them from the constants it
specifies. If a change requires violating a criterion there, stop and say so
rather than working around it.

## Known non-errors

Do not "fix" these:

- CMP0219 policy warning: SDL3's own CMake code.
- clangd consteval / `_M_bf16` squiggles on `std::println`: CLion's bundled clangd
  parsing libstdc++ headers. GCC compiles them. Deferred until CLion supports
  Clang 23's driver.
- CLion runs the gcc profile only. Clang 23 breaks CLion's compiler probe.

## Constraints

- GPU required (Vulkan or Metal). No CPU fallback. Integrated graphics is the floor.
- Frame budget 16 ms, input-to-paint under 8 ms. Check changes against this.
- Text rendering: shaping-capable pipeline with an ASCII-monospace fast path.
  Glyph atlas keyed by glyph ID plus subpixel offset, never by character.
- Decorations (inlay hints, diagnostics, agent ghost text) are first-class.
  A line produces positioned runs tagged by origin, not a string.
- SDL3 pinned via FetchContent. Do not unpin.

## Logging

Use the `STACKIDE_LOG_*` macros from `stackide/base/log.hpp`, not direct stderr writes.
Direct output is for program results and CLI errors only. Logging is asynchronous, so
call `log_flush()` before printing direct output that must follow earlier log lines.
`STACKIDE_LOG` sets the level and `STACKIDE_LOG_DIR` the directory.

## Gotchas

- `git clone` from the local path copies committed history. Commit before
  testing what CI will see; mount the working tree directly when iterating.
- Always pass `-u "$(id -u):$(id -g)"` to docker run with a mounted tree,
  or you get root-owned files in your source tree.
- The toolchain image sets `SDL_VIDEODRIVER=dummy`, which beats a normal
  `SDL_SetHint`. A mode that needs a specific video driver must set the hint with
  `SDL_HINT_OVERRIDE`. The container renders on lavapipe (software Vulkan).