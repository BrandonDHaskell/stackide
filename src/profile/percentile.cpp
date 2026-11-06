#include <stackide/profile/percentile.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace stackide
{

    double percentile(const std::vector<double>& sorted, double p)
    {
        if (sorted.empty())
        {
            return 0.0;
        }
        const auto count = static_cast<double>(sorted.size());
        const auto one_based_rank = static_cast<std::size_t>(std::clamp(std::ceil(p * count), 1.0, count));
        return sorted.at(one_based_rank - 1);
    }

} // namespace stackide
