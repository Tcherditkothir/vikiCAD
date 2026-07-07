#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "geom/GeomUtil.h"
#include "geom/Xform2d.h"

using namespace viki;
using Catch::Approx;

TEST_CASE("circle from 3 points", "[geom]")
{
    SECTION("unit circle")
    {
        const auto c = circleFrom3Points({1, 0}, {0, 1}, {-1, 0});
        REQUIRE(c);
        REQUIRE(c->center.x == Approx(0.0).margin(1e-12));
        REQUIRE(c->center.y == Approx(0.0).margin(1e-12));
        REQUIRE(c->radius == Approx(1.0));
    }
    SECTION("collinear points give no circle")
    {
        REQUIRE_FALSE(circleFrom3Points({0, 0}, {1, 1}, {2, 2}));
    }
    SECTION("offset circle")
    {
        const auto c = circleFrom3Points({15, 10}, {10, 15}, {5, 10});
        REQUIRE(c);
        REQUIRE(c->center.x == Approx(10.0));
        REQUIRE(c->center.y == Approx(10.0));
        REQUIRE(c->radius == Approx(5.0));
    }
}

TEST_CASE("distance to segment", "[geom]")
{
    REQUIRE(distanceToSegment({0, 1}, {-1, 0}, {1, 0}) == Approx(1.0));
    REQUIRE(distanceToSegment({5, 0}, {-1, 0}, {1, 0}) == Approx(4.0)); // beyond end
    REQUIRE(distanceToSegment({0, 0}, {0, 0}, {0, 0}) == Approx(0.0)); // degenerate
}

TEST_CASE("angle helpers", "[geom]")
{
    REQUIRE(normalizeAngle(-M_PI_2) == Approx(1.5 * M_PI));
    REQUIRE(ccwSweep(0, M_PI_2) == Approx(M_PI_2));
    REQUIRE(ccwSweep(M_PI_2, 0) == Approx(1.5 * M_PI));
    REQUIRE(angleOnArc(M_PI_4, 0, M_PI_2));
    REQUIRE_FALSE(angleOnArc(M_PI, 0, M_PI_2));
}

TEST_CASE("xform composition and mirror detection", "[geom]")
{
    const Xform2d rot = Xform2d::rotation(M_PI_2, {1, 0});
    const Vec2d p = rot.apply({2, 0});
    REQUIRE(p.x == Approx(1.0).margin(1e-12));
    REQUIRE(p.y == Approx(1.0).margin(1e-12));

    const Xform2d t = Xform2d::translation({3, 4});
    const Vec2d q = t.compose(rot).apply({2, 0}); // rotate then translate
    REQUIRE(q.x == Approx(4.0).margin(1e-12));
    REQUIRE(q.y == Approx(5.0).margin(1e-12));

    Xform2d mirror{-1, 0, 0, 1, 0, 0};
    REQUIRE(mirror.det() < 0);
    REQUIRE(rot.det() > 0);
}
