#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

#include "geom/Vec2d.h"

using viki::Vec2d;
using Catch::Approx;

TEST_CASE("Vec2d arithmetic", "[geom]")
{
    const Vec2d a{3.0, 4.0};
    const Vec2d b{1.0, -2.0};

    REQUIRE(nearEqual(a + b, Vec2d{4.0, 2.0}));
    REQUIRE(nearEqual(a - b, Vec2d{2.0, 6.0}));
    REQUIRE(nearEqual(a * 2.0, Vec2d{6.0, 8.0}));
    REQUIRE(a.dot(b) == Approx(-5.0));
    REQUIRE(a.cross(b) == Approx(-10.0));
    REQUIRE(a.length() == Approx(5.0));
}

TEST_CASE("Vec2d normalization and rotation", "[geom]")
{
    const Vec2d a{3.0, 4.0};
    REQUIRE(a.normalized().length() == Approx(1.0));
    REQUIRE(nearEqual(Vec2d{0.0, 0.0}.normalized(), Vec2d{0.0, 0.0}));

    SECTION("rotating twice by pi/2 equals perp of perp")
    {
        const Vec2d r = a.rotated(M_PI); // half turn
        REQUIRE(nearEqual(r, Vec2d{-3.0, -4.0}, 1e-12));
        REQUIRE(nearEqual(a.perp().perp(), Vec2d{-3.0, -4.0}));
    }

    SECTION("polar construction")
    {
        const Vec2d p = Vec2d::polar(2.0, M_PI / 2.0);
        REQUIRE(p.x == Approx(0.0).margin(1e-12));
        REQUIRE(p.y == Approx(2.0));
    }
}
