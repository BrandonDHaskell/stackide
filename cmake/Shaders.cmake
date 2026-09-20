# cmake/Shaders.cmake
#
# Compiles GLSL sources under shaders/ to SPIR-V via glslangValidator, then
# generates a C++ header embedding each shader's bytes as a static array.
# No .glsl or .spv file is installed or read at runtime (see
# docs/buildouts/M1-textured-quad.md decision 1 and 4).

include_guard(GLOBAL)

find_program(GLSLANG_VALIDATOR NAMES glslangValidator glslang)
if(NOT GLSLANG_VALIDATOR)
    message(FATAL_ERROR
            "glslangValidator (or glslang) not found. Install glslang-tools "
            "(see docs/buildouts/M1-textured-quad.md decision 1) and ensure "
            "it is on PATH before configuring.")
endif()

set(STACKIDE_SHADER_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/include")

# stackide_add_shader(<target> <source>.glsl <vert|frag> <var-name>)
#
# Compiles <source> to SPIR-V and generates a header at
# ${STACKIDE_SHADER_INCLUDE_DIR}/stackide/shaders/<basename>_spv.hpp exposing
# the bytes as `inline constexpr std::array<unsigned char, N> <var-name>`.
function(stackide_add_shader target source stage var_name)
    get_filename_component(file_name "${source}" NAME)
    string(REGEX REPLACE "\\.glsl$" "" stem "${file_name}")
    string(REPLACE "." "_" base_name "${stem}")
    set(spv_path "${CMAKE_CURRENT_BINARY_DIR}/shaders/${base_name}.spv")
    set(header_path "${STACKIDE_SHADER_INCLUDE_DIR}/stackide/shaders/${base_name}_spv.hpp")
    set(source_path "${CMAKE_CURRENT_SOURCE_DIR}/${source}")

    add_custom_command(
            OUTPUT "${spv_path}"
            COMMAND "${GLSLANG_VALIDATOR}" -V -S "${stage}" -o "${spv_path}" "${source_path}"
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
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateShaderHeader.cmake"
            DEPENDS "${spv_path}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateShaderHeader.cmake"
            COMMENT "Embedding ${base_name}.spv as ${var_name}"
            VERBATIM
    )

    target_sources(${target} PRIVATE "${header_path}")
endfunction()
