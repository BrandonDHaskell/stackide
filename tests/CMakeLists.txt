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