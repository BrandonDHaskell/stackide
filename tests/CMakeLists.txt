# tests/CMakeLists.txt
#
# Smoke tests drive the stackide binary as a black box: the process exit code
# is the assertion. No test framework is linked yet, deliberately (see notes at
# the bottom of this file).
#
# Labels:
#   smoke    every test here
#   gpu      requires a working GPU backend (Vulkan, or lavapipe in CI)
#   display  requires a window system (never runs on a stock CI runner)
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

# ---------------------------------------------------------------------------
# Tier 3: window plus swapchain. Local and self-hosted runners only.
# ---------------------------------------------------------------------------
add_test(NAME smoke.windowed
        COMMAND stackide --frames 3 --timeout-ms 10000)
set_tests_properties(smoke.windowed PROPERTIES
        LABELS "smoke;gpu;display"
        RESOURCE_LOCK gpu)
list(APPEND STACKIDE_SMOKE_TESTS smoke.windowed)

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
add_test(NAME smoke.frame_cost_gate
        COMMAND stackide --present-mode immediate --frames 1000
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
        SKIP_RETURN_CODE 77)

# ---------------------------------------------------------------------------
# Notes
#
# No Catch2 or GoogleTest yet. Add one the first time a test needs to assert on
# something other than an exit code, which will most likely be the arg parser.
# At that point, split src/ into a stackide_lib target plus a thin stackide
# executable, then link the test binary against stackide_lib and register it
# here with the same label scheme.
# ---------------------------------------------------------------------------