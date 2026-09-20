# cmake/GenerateShaderHeader.cmake
#
# Turns a compiled SPIR-V binary into a C++ header exposing its bytes as a
# static array, so no .spv file ships with the binary or is read at runtime
# (see docs/buildouts/M1-textured-quad.md decision 4). Revisit #embed once
# both GCC 16 and Clang support it in C++ mode.
#
# Usage: cmake -DINPUT_SPV=<path> -DOUTPUT_HEADER=<path> -DVAR_NAME=<identifier> \
#              -P GenerateShaderHeader.cmake

if(NOT DEFINED INPUT_SPV OR NOT DEFINED OUTPUT_HEADER OR NOT DEFINED VAR_NAME)
    message(FATAL_ERROR "GenerateShaderHeader.cmake requires INPUT_SPV, OUTPUT_HEADER, and VAR_NAME")
endif()

file(READ "${INPUT_SPV}" hex_content HEX)
string(LENGTH "${hex_content}" hex_length)
math(EXPR byte_count "${hex_length} / 2")

string(REGEX MATCHALL "[A-Fa-f0-9][A-Fa-f0-9]" byte_list "${hex_content}")
list(TRANSFORM byte_list PREPEND "0x")
list(JOIN byte_list ", " byte_literals)

file(WRITE "${OUTPUT_HEADER}" "// Generated from ${INPUT_SPV} by cmake/GenerateShaderHeader.cmake.
// Do not edit by hand.
#pragma once

#include <array>

inline constexpr std::array<unsigned char, ${byte_count}> ${VAR_NAME} = { ${byte_literals} };
")
