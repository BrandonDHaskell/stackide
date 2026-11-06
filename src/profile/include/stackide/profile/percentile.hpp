#pragma once

#include <vector>

namespace stackide
{

    // Nearest-rank percentile of an ascending sample, with p in [0, 1]. Returns an
    // observed value, and 0.0 for an empty sample.
    double percentile(const std::vector<double>& sorted, double p);

} // namespace stackide
