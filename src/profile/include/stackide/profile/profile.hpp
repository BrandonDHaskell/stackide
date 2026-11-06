#pragma once

// STACK's front for Tracy, so that it can be swapped out or compiled away. Built without
// STACKIDE_PROFILING, these expand to nothing and their arguments aren't evaluated.

// NOLINTBEGIN(cppcoreguidelines-macro-usage): zones need the caller's scope
#if STACKIDE_PROFILING

#include <tracy/Tracy.hpp>

#define STACKIDE_ZONE(name) ZoneScopedN(name)
#define STACKIDE_FRAME() FrameMark
#define STACKIDE_PLOT(name, value) TracyPlot(name, static_cast<double>(value))
#define STACKIDE_THREAD_NAME(name) tracy::SetThreadName(name)

#else

#define STACKIDE_ZONE(name)
#define STACKIDE_FRAME()
#define STACKIDE_PLOT(name, value)
#define STACKIDE_THREAD_NAME(name)

#endif
// NOLINTEND(cppcoreguidelines-macro-usage)
