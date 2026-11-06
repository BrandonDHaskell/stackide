cmake_minimum_required(VERSION 3.28)
project(stackide LANGUAGES C CXX)

# 1. Options
option(STACKIDE_HEADLESS "Build without GUI backends" OFF)
option(STACKIDE_BUILD_TESTS "Build stackide tests" ${PROJECT_IS_TOP_LEVEL})
option(STACKIDE_WERROR "Treat warnings as errors" ${PROJECT_IS_TOP_LEVEL})
option(STACKIDE_PROFILING "Compile Tracy profiling in" OFF)
set(STACKIDE_SANITIZE "" CACHE STRING "Value for -fsanitize=, empty to disable")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# The project uses headers, not modules. Scanning only adds build steps, and module map
# flags in compile_commands.json that clang-tidy can't read before they're generated.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# 2. Toolchain and dependencies
include(CompilerChecks)

# Set before the dependencies so they are instrumented too: TSan reports false
# races across uninstrumented code, and ASan misses overflows into it.
if(STACKIDE_SANITIZE)
    add_compile_options("-fsanitize=${STACKIDE_SANITIZE}" -fno-omit-frame-pointer)
    add_link_options("-fsanitize=${STACKIDE_SANITIZE}")
endif()

include(cmake/Dependencies.cmake)
include(Shaders)
include(Lint)

# 3. Modules
add_library(stackide_base STATIC
        src/base/log.cpp
        src/base/options.cpp
        src/base/include/stackide/base/bounded_queue.hpp
        src/base/include/stackide/base/bump_cursor.hpp
        src/base/include/stackide/base/constants.hpp
        src/base/include/stackide/base/log.hpp
        src/base/include/stackide/base/options.hpp
        src/base/include/stackide/base/scope_exit.hpp
)
target_include_directories(stackide_base PUBLIC src/base/include)
target_link_libraries(stackide_base
        PUBLIC Threads::Threads
        PRIVATE stackide_warnings)

# The SIMD baseline from docs/hardware-environment.md. NEON is implied on ARM64.
# Public so every stackide target agrees, and constants.hpp fails the build otherwise.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    target_compile_options(stackide_base PUBLIC -mavx2)
endif()

# A sanitizer finding has to fail the run, not scroll past it.
if(STACKIDE_SANITIZE)
    target_compile_options(stackide_base PUBLIC -fno-sanitize-recover=all)
endif()

add_library(stackide_profile STATIC
        src/profile/percentile.cpp
        src/profile/stall_detector.cpp
        src/profile/include/stackide/profile/percentile.hpp
        src/profile/include/stackide/profile/stall_detector.hpp
)
target_include_directories(stackide_profile PUBLIC src/profile/include)
target_link_libraries(stackide_profile
        PUBLIC stackide_base Tracy::TracyClient
        PRIVATE stackide_warnings)

# Never links SDL or UI code: see docs/adr/0001-ui-core-split.md.
add_library(stackide_core STATIC
        src/core/core.cpp
        src/core/messages.cpp
        src/core/worker_pool.cpp
        src/core/include/stackide/core/core.hpp
        src/core/include/stackide/core/messages.hpp
        src/core/include/stackide/core/worker_pool.hpp
)
target_include_directories(stackide_core PUBLIC src/core/include)
target_link_libraries(stackide_core
        PUBLIC stackide_base
        PRIVATE stackide_warnings)

# The only module besides render that may use SDL. Its public headers don't include it.
add_library(stackide_platform STATIC
        src/platform/event.cpp
        src/platform/main_loop.cpp
        src/platform/video.cpp
        src/platform/window.cpp
        src/platform/include/stackide/platform/event.hpp
        src/platform/include/stackide/platform/main_loop.hpp
        src/platform/include/stackide/platform/video.hpp
        src/platform/include/stackide/platform/window.hpp
)
target_include_directories(stackide_platform PUBLIC src/platform/include)
target_link_libraries(stackide_platform
        PUBLIC stackide_base stackide_core
        PRIVATE SDL3::SDL3 stackide_warnings)

add_library(stackide_render STATIC
        src/render/gpu.cpp
        src/render/quad_resources.cpp
        src/render/run_offscreen.cpp
        src/render/run_windowed.cpp
        src/render/upload_ring.cpp
        src/render/include/stackide/render/gpu.hpp
        src/render/include/stackide/render/modes.hpp
        src/render/include/stackide/render/quad_resources.hpp
        src/render/include/stackide/render/render_constants.hpp
        src/render/include/stackide/render/upload_ring.hpp
)
target_include_directories(stackide_render
        PUBLIC src/render/include
        PRIVATE "${STACKIDE_SHADER_INCLUDE_DIR}")
target_link_libraries(stackide_render
        PUBLIC stackide_base stackide_core stackide_platform
        PRIVATE SDL3::SDL3 stackide_profile stackide_warnings)

stackide_add_shader(stackide_render shaders/quad.vert.hlsl vert kQuadVertSpv)
stackide_add_shader(stackide_render shaders/quad.frag.hlsl frag kQuadFragSpv)

add_executable(stackide src/app/main.cpp)
target_link_libraries(stackide PRIVATE stackide_platform stackide_render stackide_warnings)

include(Boundaries)
foreach(sdl_free_module stackide_base stackide_profile stackide_core)
    stackide_forbid_links(${sdl_free_module} "^(-l)?SDL3|^stackide_(render|platform)$")
endforeach()

# 4. Testing Configuration
if(STACKIDE_BUILD_TESTS)
    enable_testing()

    # Smoke and unit tests. tests/CMakeLists.txt is the single source of
    # truth for test registration; do not duplicate add_test() calls here.
    add_subdirectory(tests)
endif()
