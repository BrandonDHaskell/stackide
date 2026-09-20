# cmake/CompilerChecks.cmake
#
# Not a CMAKE_TOOLCHAIN_FILE. The compiler is selected by CMakePresets.json;
# this file verifies that the selection actually delivered what the project
# requires, and probes the C++26 library features that need shims.
#
# Include from the top-level CMakeLists.txt after project().

include_guard(GLOBAL)
include(CheckCXXSourceCompiles)

# ---------------------------------------------------------------------------
# Minimum versions. Bump deliberately, with a note in docs/toolchain.md.
# ---------------------------------------------------------------------------

set(STACKIDE_MIN_GCC   16.0)
set(STACKIDE_MIN_CLANG 21.0)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS STACKIDE_MIN_GCC)
        message(FATAL_ERROR
                "stackide requires GCC ${STACKIDE_MIN_GCC} or newer.\n"
                "  found:    ${CMAKE_CXX_COMPILER_VERSION}\n"
                "  compiler: ${CMAKE_CXX_COMPILER}\n"
                "Debian 13 does not ship a new enough GCC. Configure with a preset "
                "(cmake --preset gcc) rather than relying on the system compiler.")
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS STACKIDE_MIN_CLANG)
        message(FATAL_ERROR
                "stackide requires Clang ${STACKIDE_MIN_CLANG} or newer.\n"
                "  found:    ${CMAKE_CXX_COMPILER_VERSION}\n"
                "  compiler: ${CMAKE_CXX_COMPILER}\n"
                "Configure with a preset (cmake --preset clang).")
    endif()
else()
    message(FATAL_ERROR
            "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}. "
            "stackide supports GCC and Clang only. See docs/toolchain.md.")
endif()

message(STATUS "stackide: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} "
        "(${CMAKE_CXX_COMPILER})")

# ---------------------------------------------------------------------------
# Feature probes.
#
# Version checks confirm the toolchain. These confirm the library, which on a
# bleeding-edge compiler is the part that actually lags. Each probe drives a
# shim, so a missing feature is a fallback path, not a configure failure.
# ---------------------------------------------------------------------------

set(CMAKE_REQUIRED_FLAGS "${CMAKE_CXX_FLAGS}")

check_cxx_source_compiles("
    #include <print>
    int main() { std::println(\"{}\", 1); }
" STACKIDE_HAS_STD_PRINT)

check_cxx_source_compiles("
    #include <expected>
    int main() { std::expected<int, int> e{1}; return *e - 1; }
" STACKIDE_HAS_STD_EXPECTED)

check_cxx_source_compiles("
    #include <flat_map>
    int main() { std::flat_map<int, int> m; m.emplace(1, 2); return 0; }
" STACKIDE_HAS_STD_FLAT_MAP)

check_cxx_source_compiles("
    #include <simd>
    int main() { std::simd<float> v{}; return static_cast<int>(v[0]); }
" STACKIDE_HAS_STD_SIMD)

unset(CMAKE_REQUIRED_FLAGS)

# std::print is load-bearing in main.cpp today. Treat its absence as fatal
# rather than shimming it, until there is a reason not to.
if(NOT STACKIDE_HAS_STD_PRINT)
    message(FATAL_ERROR
            "<print> is unusable with this toolchain. Check that the preset's "
            "standard library matches its compiler (libc++ for clang, libstdc++ "
            "for gcc).")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/stackide_config.h.in"
        "${CMAKE_CURRENT_BINARY_DIR}/include/stackide/config.h"
        @ONLY)

# ---------------------------------------------------------------------------
# Warning set. Applied as an interface target, not globally, so vendored
# dependencies are not held to it.
# ---------------------------------------------------------------------------

add_library(stackide_warnings INTERFACE)

target_compile_options(stackide_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wdouble-promotion
        -Wformat=2
        $<$<CXX_COMPILER_ID:GNU>:-Wduplicated-cond;-Wduplicated-branches;-Wlogical-op;-Wuseless-cast>
        $<$<BOOL:${STACKIDE_WERROR}>:-Werror>)