include(FetchContent)

FetchContent_Declare(SDL3
        URL      https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz
        URL_HASH SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68
        SYSTEM
)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO    OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC   OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR   OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA   OFF CACHE BOOL "" FORCE)
set(SDL_POWER    OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI   OFF CACHE BOOL "" FORCE)

if(STACKIDE_HEADLESS)
    set(SDL_X11     OFF CACHE BOOL "" FORCE)
    set(SDL_WAYLAND OFF CACHE BOOL "" FORCE)
    set(SDL_DUMMY   ON  CACHE BOOL "" FORCE)
    set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
endif()

FetchContent_MakeAvailable(SDL3)

# Neither project publishes a source archive as a release asset, so these are
# GitHub's generated tag archives. If GitHub's bytes ever change, the hash check
# fails at configure rather than building something unexpected.
#
# The Tracy profiler UI must match this client's protocol, so use its 0.14.1 build.
FetchContent_Declare(tracy
        URL      https://github.com/wolfpld/tracy/archive/refs/tags/v0.14.1.tar.gz
        URL_HASH SHA256=bf4af567e9c7524d07f3caa745fad02fb33bd5694f11910750382d1efbb251c1
        SYSTEM
        EXCLUDE_FROM_ALL
)

set(TRACY_STATIC ON CACHE BOOL "" FORCE)
set(TRACY_ENABLE ${STACKIDE_PROFILING} CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(tracy)

if(STACKIDE_BUILD_TESTS)
    FetchContent_Declare(Catch2
            URL      https://github.com/catchorg/Catch2/archive/refs/tags/v3.16.1.tar.gz
            URL_HASH SHA256=d16bd1b8b2364918bd472cb216394acb93bde6784890df0d91bccfb3f28d777d
            SYSTEM
            EXCLUDE_FROM_ALL
    )

    FetchContent_MakeAvailable(Catch2)
endif()