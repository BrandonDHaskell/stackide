#include <stackide/base/constants.hpp>

#include <new>

// Clang reports a different destructive interference size on ARM64 than the 128
// bytes the hardware doc specifies for Apple Silicon, so x86-64 is the only target
// where the implementation's value is usable. GCC warns on any use of this constant
// because its value can vary with -mtune, which is the point of checking it here.
#if defined(__x86_64__) && defined(__cpp_lib_hardware_interference_size)
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winterference-size"
#endif
static_assert(stackide::kCacheLineBytes == std::hardware_destructive_interference_size);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif
