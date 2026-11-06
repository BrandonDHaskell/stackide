# Checks that a --profile-frames report is valid JSON with sensible contents. Run with:
#   cmake -DBINARY=<stackide> -DFRAMES=<n> -DOUTPUT=<path> "-DARGS=<args>" -P check_profile_report.cmake
# CMake's own JSON parser rejects malformed input, so nothing else is needed here.
separate_arguments(EXTRA_ARGS UNIX_COMMAND "${ARGS}")
execute_process(
        COMMAND "${BINARY}" ${EXTRA_ARGS} --profile-frames ${FRAMES} --profile-output "${OUTPUT}"
        RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the run failed: ${result}")
endif()

file(READ "${OUTPUT}" report)
string(JSON schema GET "${report}" schema_version)
string(JSON presented GET "${report}" frames_presented)
string(JSON frames_counted GET "${report}" frame_ms count)
string(JSON p99 GET "${report}" frame_ms p99)
string(JSON peak GET "${report}" peak_rss_bytes)

if(NOT schema EQUAL 1)
    message(FATAL_ERROR "unexpected schema version: ${schema}")
endif()
if(NOT presented EQUAL FRAMES OR NOT frames_counted EQUAL FRAMES)
    message(FATAL_ERROR "expected ${FRAMES} frames, got ${presented} presented and ${frames_counted} counted")
endif()
if(NOT p99 GREATER 0)
    message(FATAL_ERROR "frame time p99 is not positive: ${p99}")
endif()
if(NOT peak GREATER 0)
    message(FATAL_ERROR "peak memory is not positive: ${peak}")
endif()
message(STATUS "report ok: ${presented} frames, p99 ${p99} ms, peak ${peak} bytes")
