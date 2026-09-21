#pragma once

// Declares the run-mode entry points that are called cross-TU from main.cpp.
// run_headless() is deliberately not declared here: it's never called
// outside main.cpp, so it stays anonymous-namespace-local there.

#include <stackide/options.hpp>

namespace stackide {

int run_offscreen(const Options& opts);
int run_windowed(const Options& opts);

} // namespace stackide
