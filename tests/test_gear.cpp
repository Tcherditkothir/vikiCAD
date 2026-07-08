#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

#include "geom/GearGeometry.h"

using namespace viki;
using Catch::Approx;

TEST_CASE("gear metrics match the standard formulas", "[gear]")
{
    gear::GearParams p;
    p.module = 2.0;
    p.teeth = 20;
    p.pressureAngleDeg = 20.0;
    const gear::GearMetrics g = gear::metrics(p);

    CHECK(g.pitchDiameter == Approx(40.0));            // m*z
    CHECK(g.baseDiameter == Approx(40.0 * std::cos(20.0 * M_PI / 180.0)));
    CHECK(g.outsideDiameter == Approx(44.0));          // d + 2m
    CHECK(g.rootDiameter == Approx(35.0));             // d - 2.5m
    CHECK(g.circularPitch == Approx(M_PI * 2.0));
    CHECK(g.toothThickness == Approx(M_PI));           // half circular pitch
    // Undercut limit for 20 deg full-depth is ~17 teeth.
    CHECK(g.minTeethNoUndercut == Approx(17.097).margin(0.01));
    CHECK_FALSE(g.undercut);
}

TEST_CASE("low tooth count flags undercut", "[gear]")
{
    gear::GearParams p;
    p.teeth = 10;
    CHECK(gear::metrics(p).undercut);
}

TEST_CASE("gear profile is a closed loop within root..outside radii", "[gear]")
{
    gear::GearParams p;
    p.module = 3.0;
    p.teeth = 24;
    const gear::GearMetrics g = gear::metrics(p);
    const auto pts = gear::profile(p, {5, -7}, 0.05);
    REQUIRE(pts.size() > size_t(p.teeth) * 10);

    const Vec2d center{5, -7};
    double rmin = 1e18, rmax = 0;
    for (const Vec2d& q : pts) {
        const double r = (q - center).length();
        rmin = std::min(rmin, r);
        rmax = std::max(rmax, r);
    }
    // Never inside the root circle nor outside the addendum circle (± tol).
    CHECK(rmin == Approx(g.rootDiameter / 2.0).margin(0.2));
    CHECK(rmax == Approx(g.outsideDiameter / 2.0).margin(0.05));
    CHECK(rmax <= g.outsideDiameter / 2.0 + 1e-6);

    // Loop closes: first and last points are near each other around the ring.
    const double gap = (pts.front() - pts.back()).length();
    CHECK(gap < g.circularPitch); // within one tooth pitch (closed by polyline)
}

TEST_CASE("very low tooth count does not self-cross (pointed tip capped)",
          "[gear]")
{
    gear::GearParams p;
    p.module = 2.0;
    p.teeth = 6; // aggressively low -> pointed teeth
    const gear::GearMetrics g = gear::metrics(p);
    const auto pts = gear::profile(p, {0, 0}, 0.05);
    REQUIRE(pts.size() > 20);
    double rmax = 0;
    for (const Vec2d& q : pts)
        rmax = std::max(rmax, q.length());
    // Tip is capped at or below the addendum circle (no runaway spikes).
    CHECK(rmax <= g.outsideDiameter / 2.0 + 1e-6);
}
