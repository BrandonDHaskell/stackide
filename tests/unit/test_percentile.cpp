#include <stackide/profile/percentile.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_CASE("percentile of an empty sample is zero") { CHECK(stackide::percentile({}, 0.99) == 0.0); }

TEST_CASE("percentile uses the nearest rank")
{
    const std::vector<double> sample{1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0};

    CHECK(stackide::percentile(sample, 0.50) == 5.0);
    CHECK(stackide::percentile(sample, 0.91) == 10.0);
    CHECK(stackide::percentile(sample, 1.00) == 10.0);
}

// A p99 that interpolates down from the worst frame would hide it in a short run.
TEST_CASE("p99 of a small sample is its worst value")
{
    const std::vector<double> sample{1.0, 1.0, 1.0, 1.0, 50.0};

    CHECK(stackide::percentile(sample, 0.99) == 50.0);
}

TEST_CASE("percentile clamps to the observed range")
{
    const std::vector<double> sample{3.0, 4.0};

    CHECK(stackide::percentile(sample, 0.0) == 3.0);
    CHECK(stackide::percentile(sample, 1.5) == 4.0);
}
