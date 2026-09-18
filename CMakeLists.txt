cmake_minimum_required(VERSION 3.28)
project(stackide LANGUAGES C CXX)

option(STACKIDE_HEADLESS "Build without GUI backends" OFF)

include(cmake/Dependencies.cmake)

add_executable(stackide src/stackide/main.cpp)
target_link_libraries(stackide PRIVATE SDL3::SDL3)
target_include_directories(stackide PRIVATE include)

enable_testing()
add_test(NAME runs COMMAND stackide)