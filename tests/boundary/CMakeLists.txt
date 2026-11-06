# Fixture for the boundary tests in tests/CMakeLists.txt: fake targets, no compiler.
cmake_minimum_required(VERSION 3.28)
project(boundary_fixture LANGUAGES NONE)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/Boundaries.cmake")

add_library(core INTERFACE)
add_library(helper INTERFACE)
add_library(forbidden INTERFACE)

if(CASE STREQUAL "clean")
    target_link_libraries(core INTERFACE helper)
elseif(CASE STREQUAL "direct")
    target_link_libraries(core INTERFACE forbidden)
elseif(CASE STREQUAL "transitive")
    target_link_libraries(helper INTERFACE forbidden)
    target_link_libraries(core INTERFACE helper)
elseif(CASE STREQUAL "link_only")
    target_link_libraries(helper INTERFACE forbidden)
    target_link_libraries(core INTERFACE "$<LINK_ONLY:helper>")
elseif(CASE STREQUAL "alias")
    add_library(helper_alias ALIAS helper)
    target_link_libraries(helper INTERFACE forbidden)
    target_link_libraries(core INTERFACE helper_alias)
else()
    message(FATAL_ERROR "Unknown CASE: ${CASE}")
endif()

stackide_forbid_links(core "^forbidden")
