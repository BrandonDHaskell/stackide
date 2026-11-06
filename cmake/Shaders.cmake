# cmake/Shaders.cmake
#
# Compiles HLSL sources under shaders/ to SPIR-V via glslangValidator, then
# generates a C++ header embedding each shader's bytes as a static array.
# No .hlsl or .spv file is installed or read at runtime. See
# docs/adr/0002-hlsl-via-glslang.md for how shaders must declare their resources.

include_guard(GLOBAL)

find_program(GLSLANG_VALIDATOR NAMES glslangValidator glslang)
if(NOT GLSLANG_VALIDATOR)
    message(FATAL_ERROR
            "glslangValidator (or glslang) not found. Install glslang-tools "
            "(see docs/buildouts/M1-textured-quad.md decision 1) and ensure "
            "it is on PATH before configuring.")
endif()

set(STACKIDE_SHADER_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/include")
set(STACKIDE_GENERATE_SHADER_HEADER "${CMAKE_CURRENT_LIST_DIR}/GenerateShaderHeader.cmake")

# stackide_add_shader(<target> <source>.hlsl <vert|frag> <var-name>)
#
# Compiles <source> to SPIR-V and generates a header at
# ${STACKIDE_SHADER_INCLUDE_DIR}/stackide/shaders/<basename>_spv.hpp exposing
# the bytes as `inline constexpr std::array<unsigned char, N> <var-name>`.
function(stackide_add_shader target source stage var_name)
    get_filename_component(file_name "${source}" NAME)
    string(REGEX REPLACE "\\.hlsl$" "" stem "${file_name}")
    string(REPLACE "." "_" base_name "${stem}")
    set(spv_path "${CMAKE_CURRENT_BINARY_DIR}/shaders/${base_name}.spv")
    set(header_path "${STACKIDE_SHADER_INCLUDE_DIR}/stackide/shaders/${base_name}_spv.hpp")
    set(source_path "${CMAKE_CURRENT_SOURCE_DIR}/${source}")

    add_custom_command(
            OUTPUT "${spv_path}"
            COMMAND "${GLSLANG_VALIDATOR}" -V -D -e main -S "${stage}" -o "${spv_path}" "${source_path}"
            DEPENDS "${source_path}"
            COMMENT "Compiling ${source} to SPIR-V"
            VERBATIM
    )

    add_custom_command(
            OUTPUT "${header_path}"
            COMMAND "${CMAKE_COMMAND}"
                    "-DINPUT_SPV=${spv_path}"
                    "-DOUTPUT_HEADER=${header_path}"
                    "-DVAR_NAME=${var_name}"
                    -P "${STACKIDE_GENERATE_SHADER_HEADER}"
            DEPENDS "${spv_path}" "${STACKIDE_GENERATE_SHADER_HEADER}"
            COMMENT "Embedding ${base_name}.spv as ${var_name}"
            VERBATIM
    )

    target_sources(${target} PRIVATE "${header_path}")
endfunction()
