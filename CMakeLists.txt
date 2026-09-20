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

# 3. Core Targets
add_executable(stackide
        src/stackide/main.cpp
        include/stackide/log.hpp
)
target_link_libraries(stackide PRIVATE SDL3::SDL3)
target_include_directories(stackide PRIVATE include)

# add_subdirectory(src)

# 4. Testing Configuration
if(STACKIDE_BUILD_TESTS)
    enable_testing()

    # Unit tests in subdirectories
    add_subdirectory(tests)

    # Integration / Smoke tests
    add_test(NAME smoke_headless  COMMAND stackide --headless)
    add_test(NAME smoke_offscreen COMMAND stackide --offscreen --allow-skip)
    add_test(NAME smoke_windowed  COMMAND stackide --frames 3 --timeout-ms 10000)
    add_test(NAME runs            COMMAND stackide --smoke)

    set_tests_properties(smoke_headless smoke_offscreen smoke_windowed
            PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
    set_tests_properties(smoke_windowed PROPERTIES LABELS "gpu;display")
endif()