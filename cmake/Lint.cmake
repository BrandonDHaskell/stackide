# cmake/Lint.cmake
#
# `format-check` and `tidy` are explicit targets, not part of the default build,
# so a local build isn't slowed down. tidy needs the clang preset: it parses with
# libc++, and the generated shader headers come from depending on the libraries.

include_guard(GLOBAL)

find_program(STACKIDE_CLANG_FORMAT clang-format)
find_program(STACKIDE_CLANG_TIDY clang-tidy)

file(GLOB_RECURSE STACKIDE_LINT_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
file(GLOB_RECURSE STACKIDE_TIDY_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

if(STACKIDE_CLANG_FORMAT)
    add_custom_target(format-check
            COMMAND "${STACKIDE_CLANG_FORMAT}" --style=file --dry-run -Werror ${STACKIDE_LINT_SOURCES}
            COMMENT "Checking formatting"
            VERBATIM)
endif()

if(STACKIDE_CLANG_TIDY)
    add_custom_target(tidy
            COMMAND "${STACKIDE_CLANG_TIDY}" -p "${CMAKE_BINARY_DIR}" --quiet --warnings-as-errors=* ${STACKIDE_TIDY_SOURCES}
            COMMENT "Running clang-tidy"
            VERBATIM)
    add_dependencies(tidy stackide)
endif()
