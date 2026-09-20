cmake_minimum_required(VERSION 3.28)
project(stackide LANGUAGES C CXX)

# 1. Options
option(STACKIDE_HEADLESS "Build without GUI backends" OFF)
option(STACKIDE_BUILD_TESTS "Build stackide tests" ${PROJECT_IS_TOP_LEVEL})
option(STACKIDE_WERROR "Treat warnings as errors" ${PROJECT_IS_TOP_LEVEL})

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# 2. Dependencies
include(CompilerChecks)
include(cmake/Dependencies.cmake)
include(Shaders)

# 3. Core Targets
add_executable(stackide
        src/stackide/main.cpp
        src/stackide/options.cpp
        src/stackide/quad_resources.cpp
        src/stackide/run_offscreen.cpp
        src/stackide/run_windowed.cpp
        include/stackide/constants.hpp
        include/stackide/log.hpp
        include/stackide/modes.hpp
        include/stackide/options.hpp
        include/stackide/quad_resources.hpp
        include/stackide/scope_exit.hpp
)
target_link_libraries(stackide PRIVATE SDL3::SDL3 stackide_warnings)
target_include_directories(stackide PRIVATE include "${STACKIDE_SHADER_INCLUDE_DIR}")

stackide_add_shader(stackide shaders/quad.vert.glsl vert kQuadVertSpv)
stackide_add_shader(stackide shaders/quad.frag.glsl frag kQuadFragSpv)

# add_subdirectory(src)

# 4. Testing Configuration
if(STACKIDE_BUILD_TESTS)
    enable_testing()

    # Smoke and unit tests. tests/CMakeLists.txt is the single source of
    # truth for test registration; do not duplicate add_test() calls here.
    add_subdirectory(tests)
endif()