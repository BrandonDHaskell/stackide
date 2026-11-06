#include <stackide/profile/sample_window.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using stackide::SampleWindow;

TEST_CASE("a sample window starts empty")
{
    const SampleWindow window{4};
    CHECK(window.size() == 0);
    CHECK(window.sorted().empty());
}

TEST_CASE("a sample window sorts what it holds")
{
    SampleWindow window{4};
    window.add(3.0);
    window.add(1.0);
    CHECK(window.size() == 2);
    CHECK(window.sorted() == std::vector<double>{1.0, 3.0});
}

TEST_CASE("a full sample window drops its oldest samples")
{
    SampleWindow window{3};
    for (const double sample : {1.0, 2.0, 3.0, 4.0, 5.0})
    {
        window.add(sample);
    }
    CHECK(window.size() == 3);
    CHECK(window.sorted() == std::vector<double>{3.0, 4.0, 5.0});
}
