# tests/CMakeLists.txt
#
# Smoke tests drive the stackide binary as a black box: the process exit code
# is the assertion. Unit tests use Catch2 and sit at the bottom of this file.
#
# Labels:
#   smoke    every test here
#   gpu      requires a working GPU backend (Vulkan, or lavapipe in CI)
#   display  requires a window system (never runs on a stock CI runner)
#   unit     Catch2 tests with no GPU or window
#   build    checks on the build system itself
#   gate     performance gate that needs Reference hardware, never run in CI
#
# Exit code 77 means "skipped" and is honored via SKIP_RETURN_CODE below.

set(STACKIDE_SMOKE_TIMEOUT 30
        CACHE STRING "Wall-clock timeout, in seconds, for each smoke test")

set(STACKIDE_SMOKE_TESTS "")

# ---------------------------------------------------------------------------
# Tier 1: lifecycle only. No GPU device, no window. Runs anywhere.
# ---------------------------------------------------------------------------
add_test(NAME smoke.headless
        COMMAND stackide --headless)
set_tests_properties(smoke.headless PROPERTIES
        LABELS "smoke")
list(APPEND STACKIDE_SMOKE_TESTS smoke.headless)

# ---------------------------------------------------------------------------
# Tier 2: GPU device with no window. Clears a texture, reads it back, asserts
# the pixels. This is the tier that verifies rendering rather than linkage.
# ---------------------------------------------------------------------------
add_test(NAME smoke.offscreen
        COMMAND stackide --offscreen --allow-skip)
set_tests_properties(smoke.offscreen PROPERTIES
        LABELS "smoke;gpu"
        RESOURCE_LOCK gpu)
list(APPEND STACKIDE_SMOKE_TESTS smoke.offscreen)

# Skips where validation layers aren't installed, unless STACKIDE_REQUIRE_GPU makes that
# a failure, so that a passing run in CI really did validate.
add_test(NAME smoke.offscreen_validated
        COMMAND stackide --offscreen --gpu-debug --require-validation --allow-skip)
set_tests_properties(smoke.offscreen_validated PROPERTIES
        LABELS "smoke;gpu"
        RESOURCE_LOCK gpu)
list(APPEND STACKIDE_SMOKE_TESTS smoke.offscreen_validated)

# STACKIDE_REQUIRE_GPU must turn a skip into a failure. Checks for exit 1
# exactly, because WILL_FAIL would also accept the skip code this guards against.
add_test(NAME smoke.require_gpu_rejects_skip
        COMMAND sh -c "\"$<TARGET_FILE:stackide>\" --offscreen --allow-skip; test $? -eq 1")
set_tests_properties(smoke.require_gpu_rejects_skip PROPERTIES
        LABELS "smoke"
        ENVIRONMENT "STACKIDE_REQUIRE_GPU=1;SDL_GPU_DRIVER=stackide-no-such-driver")
list(APPEND STACKIDE_SMOKE_TESTS smoke.require_gpu_rejects_skip)

# ---------------------------------------------------------------------------
# Tier 3: window plus swapchain. Local and self-hosted runners only.
# ---------------------------------------------------------------------------
add_test(NAME smoke.windowed
        COMMAND stackide --frames 3 --timeout-ms 10000)
set_tests_properties(smoke.windowed PROPERTIES
        LABELS "smoke;gpu;display"
        RESOURCE_LOCK gpu)
list(APPEND STACKIDE_SMOKE_TESTS smoke.windowed)

foreach(driver wayland x11)
    add_test(NAME smoke.windowed_${driver}
            COMMAND stackide --frames 3 --timeout-ms 10000 --video-driver ${driver} --allow-skip)
    set_tests_properties(smoke.windowed_${driver} PROPERTIES
            LABELS "smoke;gpu;display"
            RESOURCE_LOCK gpu)
    list(APPEND STACKIDE_SMOKE_TESTS smoke.windowed_${driver})
endforeach()

# M1 item 8: a programmatic resize mid-run must not crash. Aspect-ratio
# correctness itself is a manual/visual check, not asserted here.
add_test(NAME smoke.resize
        COMMAND stackide --frames 30 --timeout-ms 10000
                --resize-after-frames 10 --resize-width 640 --resize-height 480)
set_tests_properties(smoke.resize PROPERTIES
        LABELS "smoke;gpu;display"
        RESOURCE_LOCK gpu)
list(APPEND STACKIDE_SMOKE_TESTS smoke.resize)

# ---------------------------------------------------------------------------
# Tier 4: timing gates (M1 items 6/7). Local/self-hosted hardware only, same
# as Tier 3 -- both the frame-cost floor and the live refresh rate are
# properties of real hardware, not something CI's headless container has.
# ---------------------------------------------------------------------------
# Pinned to X11: a Wayland compositor throttles immediate mode to the refresh rate, so
# there this would measure the compositor rather than the cost of a frame.
add_test(NAME smoke.frame_cost_gate
        COMMAND stackide --present-mode immediate --frames 1000 --video-driver x11
                --assert-p99-under-ms 2.0 --allow-skip)
set_tests_properties(smoke.frame_cost_gate PROPERTIES
        LABELS "smoke;gpu;display"
        RESOURCE_LOCK gpu)
list(APPEND STACKIDE_SMOKE_TESTS smoke.frame_cost_gate)

# Zero dropped frames requires a quiet desktop: this checks the render loop's
# own cadence, not the desktop's. A busy machine (heavy browser/IDE/system
# monitor load) can itself induce scheduler contention that shows up here as
# a dropped frame with no code-level cause -- close background load first if
# this fails and the number is small (1-2 over 600).
add_test(NAME smoke.cadence_gate
        COMMAND stackide --present-mode vsync --frames 600
                --assert-max-dropped-frames 0)
set_tests_properties(smoke.cadence_gate PROPERTIES
        LABELS "smoke;gpu;display"
        RESOURCE_LOCK gpu)
list(APPEND STACKIDE_SMOKE_TESTS smoke.cadence_gate)

# ---------------------------------------------------------------------------
# Frame scheduler (#8), in the real loop. All local/self-hosted, like the gates above.
# ---------------------------------------------------------------------------
add_test(NAME smoke.idle_draws_nothing
        COMMAND stackide --idle-ms 2000)
add_test(NAME smoke.timed_redraw
        COMMAND stackide --idle-ms 2000 --redraw-every-ms 100)
add_test(NAME smoke.animation_within_budget
        COMMAND stackide --frames 600 --assert-within-budget-percent 99)
# On demand and at a fast typing speed, as typing into an idle editor would be. Synthetic
# input skips the OS and the compositor, so this is the time STACK itself takes, not what a
# keyboard would see. Faster input, or an animation, also waits for the display's next
# refresh, which this target doesn't cover: see docs/hardware-environment.md.
add_test(NAME smoke.input_to_paint
        COMMAND stackide --inject-input 100 --inject-interval-ms 50 --assert-input-to-paint-under-ms 8)
add_test(NAME smoke.late_input_beats_naive
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/compare_input_sampling.sh" "$<TARGET_FILE:stackide>")
set_tests_properties(
        smoke.idle_draws_nothing smoke.timed_redraw smoke.animation_within_budget smoke.input_to_paint
        smoke.late_input_beats_naive
        PROPERTIES
        LABELS "smoke;gpu;display"
        RESOURCE_LOCK gpu)

# The stall detector has to be wired into the real loop, not just work in isolation.
add_test(NAME smoke.stall_detected
        COMMAND sh -c "d=$(mktemp -d '${CMAKE_CURRENT_BINARY_DIR}/logs-check.XXXXXX') && STACKIDE_LOG_DIR=$d '$<TARGET_FILE:stackide>' --frames 5 --inject-stall-ms 50 --timeout-ms 10000 && grep -q 'main loop stall.*injected stall.*run_windowed.cpp' $d/stackide.log")
set_tests_properties(smoke.stall_detected PROPERTIES
        LABELS "smoke;gpu;display"
        RESOURCE_LOCK gpu)

# ---------------------------------------------------------------------------
# Negative tests: bad input must fail loudly, not fall back to a default.
# ---------------------------------------------------------------------------
add_test(NAME smoke.rejects_unknown_flag
        COMMAND stackide --nonsense)
set_tests_properties(smoke.rejects_unknown_flag PROPERTIES
        LABELS "smoke"
        WILL_FAIL TRUE)
list(APPEND STACKIDE_SMOKE_TESTS smoke.rejects_unknown_flag)

add_test(NAME smoke.rejects_missing_frame_count
        COMMAND stackide --frames)
set_tests_properties(smoke.rejects_missing_frame_count PROPERTIES
        LABELS "smoke"
        WILL_FAIL TRUE)
list(APPEND STACKIDE_SMOKE_TESTS smoke.rejects_missing_frame_count)

add_test(NAME smoke.rejects_bad_present_mode
        COMMAND stackide --present-mode bogus)
set_tests_properties(smoke.rejects_bad_present_mode PROPERTIES
        LABELS "smoke"
        WILL_FAIL TRUE)
list(APPEND STACKIDE_SMOKE_TESTS smoke.rejects_bad_present_mode)

add_test(NAME smoke.rejects_negative_assert_p99
        COMMAND stackide --assert-p99-under-ms -1.0 --present-mode immediate)
set_tests_properties(smoke.rejects_negative_assert_p99 PROPERTIES
        LABELS "smoke"
        WILL_FAIL TRUE)
list(APPEND STACKIDE_SMOKE_TESTS smoke.rejects_negative_assert_p99)

add_test(NAME smoke.rejects_assert_p99_in_offscreen
        COMMAND stackide --offscreen --assert-p99-under-ms 2.0)
set_tests_properties(smoke.rejects_assert_p99_in_offscreen PROPERTIES
        LABELS "smoke"
        WILL_FAIL TRUE)
list(APPEND STACKIDE_SMOKE_TESTS smoke.rejects_assert_p99_in_offscreen)

add_test(NAME smoke.rejects_assert_p99_in_vsync
        COMMAND stackide --assert-p99-under-ms 2.0)
set_tests_properties(smoke.rejects_assert_p99_in_vsync PROPERTIES
        LABELS "smoke"
        WILL_FAIL TRUE)
list(APPEND STACKIDE_SMOKE_TESTS smoke.rejects_assert_p99_in_vsync)

add_test(NAME smoke.rejects_dropped_gate_in_immediate
        COMMAND stackide --present-mode immediate --assert-max-dropped-frames 0)
set_tests_properties(smoke.rejects_dropped_gate_in_immediate PROPERTIES
        LABELS "smoke"
        WILL_FAIL TRUE)
list(APPEND STACKIDE_SMOKE_TESTS smoke.rejects_dropped_gate_in_immediate)

add_test(NAME smoke.rejects_partial_resize_flags
        COMMAND stackide --resize-after-frames 10)
set_tests_properties(smoke.rejects_partial_resize_flags PROPERTIES
        LABELS "smoke"
        WILL_FAIL TRUE)
list(APPEND STACKIDE_SMOKE_TESTS smoke.rejects_partial_resize_flags)

add_test(NAME smoke.rejects_resize_in_offscreen
        COMMAND stackide --offscreen --resize-after-frames 10
                --resize-width 640 --resize-height 480)
set_tests_properties(smoke.rejects_resize_in_offscreen PROPERTIES
        LABELS "smoke"
        WILL_FAIL TRUE)
list(APPEND STACKIDE_SMOKE_TESTS smoke.rejects_resize_in_offscreen)

# ---------------------------------------------------------------------------
# Properties shared by every test above.
# ---------------------------------------------------------------------------
set_tests_properties(${STACKIDE_SMOKE_TESTS} PROPERTIES
        TIMEOUT ${STACKIDE_SMOKE_TIMEOUT}
        SKIP_RETURN_CODE 77
        ENVIRONMENT_MODIFICATION "STACKIDE_LOG_DIR=set:${CMAKE_CURRENT_BINARY_DIR}/logs")

# The log file must really be written by a normal run. A new directory each time, so
# that an earlier run can't satisfy the check.
add_test(NAME smoke.log_file_written
        COMMAND sh -c "d=$(mktemp -d '${CMAKE_CURRENT_BINARY_DIR}/logs-check.XXXXXX') && STACKIDE_LOG_DIR=$d '$<TARGET_FILE:stackide>' --headless && grep -q 'video driver = dummy' $d/stackide.log")
set_tests_properties(smoke.log_file_written PROPERTIES LABELS "smoke")

# ---------------------------------------------------------------------------
# Unit tests. No GPU or window, so these also run under TSan, where uninstrumented
# drivers make the GPU tests unusable.
# ---------------------------------------------------------------------------
# Long enough for a sanitizer on a slow runner. A concurrency bug can show up as a hang,
# which should fail the test rather than stall CI.
set(STACKIDE_UNIT_TIMEOUT 300
        CACHE STRING "Wall-clock timeout, in seconds, for each unit test binary")

add_executable(stackide_unit_tests
        unit/test_bounded_queue.cpp
        unit/test_bump_cursor.cpp
        unit/test_constants.cpp
        unit/test_core.cpp
        unit/test_log.cpp
        unit/test_log_compile_level.cpp
        unit/test_options.cpp
        unit/test_percentile.cpp
        unit/test_sample_window.cpp
        unit/test_stall_detector.cpp
        unit/test_worker_pool.cpp)
target_link_libraries(stackide_unit_tests PRIVATE
        stackide_base stackide_core stackide_profile stackide_warnings Catch2::Catch2WithMain)
add_test(NAME unit COMMAND stackide_unit_tests)
set_tests_properties(unit PROPERTIES LABELS "unit" TIMEOUT ${STACKIDE_UNIT_TIMEOUT})

# Uses SDL's dummy video backend, so it needs no display or GPU and can run under TSan.
add_executable(stackide_main_loop_tests platform/test_frame_scheduler.cpp platform/test_main_loop.cpp)
target_link_libraries(stackide_main_loop_tests PRIVATE
        stackide_core stackide_platform SDL3::SDL3 stackide_warnings Catch2::Catch2WithMain)
add_test(NAME unit.main_loop COMMAND stackide_main_loop_tests)
set_tests_properties(unit.main_loop PROPERTIES LABELS "unit" TIMEOUT ${STACKIDE_UNIT_TIMEOUT})

# ---------------------------------------------------------------------------
# Module boundary check (docs/adr/0001-ui-core-split.md), run against fake targets.
# Rejections match the error text, since any configure failure would satisfy
# WILL_FAIL, including a broken fixture.
# ---------------------------------------------------------------------------
function(stackide_add_boundary_test case)
    add_test(NAME boundary.${case}
            COMMAND "${CMAKE_COMMAND}" --fresh -G "${CMAKE_GENERATOR}" -S "${CMAKE_CURRENT_SOURCE_DIR}/boundary"
                    -B "${CMAKE_CURRENT_BINARY_DIR}/boundary/${case}" -DCASE=${case})
    set_tests_properties(boundary.${case} PROPERTIES LABELS "build" ${ARGN})
endfunction()

stackide_add_boundary_test(clean)
foreach(case direct transitive link_only alias)
    stackide_add_boundary_test(${case} PASS_REGULAR_EXPRESSION "core must not link")
endforeach()

# ---------------------------------------------------------------------------
# Tests that need a real GPU device, on lavapipe in CI.
# ---------------------------------------------------------------------------
add_executable(stackide_gpu_tests gpu/test_upload_ring.cpp)
target_link_libraries(stackide_gpu_tests PRIVATE
        stackide_render stackide_platform SDL3::SDL3 stackide_warnings Catch2::Catch2WithMain)
add_test(NAME gpu.upload_ring COMMAND stackide_gpu_tests)
set_tests_properties(gpu.upload_ring PROPERTIES
        LABELS "gpu"
        RESOURCE_LOCK gpu
        SKIP_REGULAR_EXPRESSION "skipped: no GPU device")

# The validation gate has to be able to fail: a shader SDL_GPU can't use, on a real
# device, must make a --gpu-debug result a failure. Its own binary because the upload
# ring test must set SDL's memory functions before anything else touches SDL.
add_executable(stackide_gpu_validation_tests gpu/test_validation.cpp)
target_include_directories(stackide_gpu_validation_tests PRIVATE "${STACKIDE_SHADER_INCLUDE_DIR}")
target_link_libraries(stackide_gpu_validation_tests PRIVATE
        stackide_render stackide_platform SDL3::SDL3 stackide_warnings Catch2::Catch2WithMain)
stackide_add_shader(stackide_gpu_validation_tests gpu/bad_binding.frag.hlsl frag kBadBindingFragSpv)
add_dependencies(stackide_gpu_validation_tests stackide_render)
add_test(NAME gpu.validation_gate COMMAND stackide_gpu_validation_tests)
set_tests_properties(gpu.validation_gate PROPERTIES
        LABELS "gpu"
        RESOURCE_LOCK gpu
        SKIP_REGULAR_EXPRESSION "skipped: validation layers not installed")

# ---------------------------------------------------------------------------
# Log call cost (#152). The gate only means something in an optimized, uninstrumented
# build on Reference hardware, so it is labeled to stay out of CI, which runs the
# benchmark just to record the number.
# ---------------------------------------------------------------------------
add_executable(stackide_log_bench bench/log_bench.cpp)
target_link_libraries(stackide_log_bench PRIVATE stackide_base stackide_warnings)

if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT STACKIDE_SANITIZE)
    add_test(NAME bench.log_call_gate COMMAND stackide_log_bench --assert-under-ns 50)
    set_tests_properties(bench.log_call_gate PROPERTIES LABELS "gate")
endif()
