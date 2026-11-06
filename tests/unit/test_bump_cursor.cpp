#include <stackide/base/bump_cursor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

using stackide::BumpCursor;

TEST_CASE("reservations are consecutive and aligned")
{
    BumpCursor cursor{100};
    CHECK(cursor.reserve(3) == 0U);
    CHECK(cursor.reserve(4, 8) == 8U);
    CHECK(cursor.reserve(1) == 12U);
    CHECK(cursor.used() == 13);
}

TEST_CASE("a request that doesn't fit changes nothing")
{
    BumpCursor cursor{10};
    REQUIRE(cursor.reserve(8));
    CHECK_FALSE(cursor.reserve(3));
    CHECK(cursor.used() == 8);
    CHECK(cursor.reserve(2) == 8U);
    CHECK_FALSE(cursor.reserve(0, 16));
}

TEST_CASE("alignment padding counts against capacity")
{
    BumpCursor cursor{16};
    REQUIRE(cursor.reserve(1));
    CHECK_FALSE(cursor.reserve(9, 8));
    CHECK(cursor.reserve(8, 8) == 8U);
}

TEST_CASE("sizes near the limit can't wrap around")
{
    constexpr auto kMax = std::numeric_limits<std::uint32_t>::max();
    BumpCursor cursor{kMax};
    REQUIRE(cursor.reserve(kMax - 1));
    CHECK_FALSE(cursor.reserve(2));
    CHECK_FALSE(cursor.reserve(kMax, 4));
}

TEST_CASE("reset starts over")
{
    BumpCursor cursor{4};
    REQUIRE(cursor.reserve(4));
    cursor.reset();
    CHECK(cursor.used() == 0);
    CHECK(cursor.reserve(4) == 0U);
}
