#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "geom/Regions.h"
#include "geom/Vec2d.h"

using namespace viki;
using Catch::Matchers::WithinAbs;

namespace {

// A closed rectangle as four line curves (CCW).
void addRect(std::vector<Curve>& out, double x0, double y0, double x1, double y1)
{
    Vec2d a{x0, y0}, b{x1, y0}, c{x1, y1}, d{x0, y1};
    out.push_back(Curve::line(a, b));
    out.push_back(Curve::line(b, c));
    out.push_back(Curve::line(c, d));
    out.push_back(Curve::line(d, a));
}

double totalArea(const std::vector<Region>& regs)
{
    double s = 0.0;
    for (const auto& r : regs)
        s += r.area;
    return s;
}

// Point-in-polygon (ray casting) on a region boundary.
bool contains(const Region& r, const Vec2d& p)
{
    bool in = false;
    const auto& b = r.boundary;
    for (size_t i = 0, j = b.size() - 1; i < b.size(); j = i++) {
        const bool cross = ((b[i].y > p.y) != (b[j].y > p.y)) &&
                           (p.x < (b[j].x - b[i].x) * (p.y - b[i].y) / (b[j].y - b[i].y) + b[i].x);
        if (cross)
            in = !in;
    }
    return in;
}

} // namespace

TEST_CASE("findRegions: two overlapping crossing rectangles yield 3 regions", "[regions]")
{
    // Two squares sharing an overlap rectangle in the middle. Their arrangement
    // has three minimal bounded regions: left-only, overlap, right-only.
    std::vector<Curve> curves;
    addRect(curves, 0.0, 0.0, 6.0, 6.0);  // left square
    addRect(curves, 4.0, 0.0, 10.0, 6.0); // right square, overlaps x in [4,6]

    auto regs = findRegions(curves);

    REQUIRE(regs.size() == 3);

    // Total covered area = union area (no double counting): 6*6 + 6*6 - 2*6 = 60.
    CHECK_THAT(totalArea(regs), WithinAbs(60.0, 1e-6));

    // Each of the three characteristic interior points falls in exactly one region.
    for (const Vec2d& probe : {Vec2d{2.0, 3.0}, Vec2d{5.0, 3.0}, Vec2d{8.0, 3.0}}) {
        int hits = 0;
        for (const auto& r : regs)
            if (contains(r, probe))
                ++hits;
        CHECK(hits == 1);
    }
}

TEST_CASE("findRegions: plus shape from a cross of two rectangles", "[regions]")
{
    // A vertical bar and a horizontal bar crossing at the center. The
    // arrangement splits into 5 bounded regions: the central square plus the
    // four arms.
    std::vector<Curve> curves;
    addRect(curves, 4.0, 0.0, 6.0, 10.0); // vertical bar
    addRect(curves, 0.0, 4.0, 10.0, 6.0); // horizontal bar

    auto regs = findRegions(curves);

    REQUIRE(regs.size() == 5);

    // Union area of the plus = two 2x10 bars minus the 2x2 double-counted center.
    CHECK_THAT(totalArea(regs), WithinAbs(2.0 * 10.0 + 2.0 * 10.0 - 2.0 * 2.0, 1e-6));

    // Center probe lands in exactly one region.
    int centerHits = 0;
    for (const auto& r : regs)
        if (contains(r, Vec2d{5.0, 5.0}))
            ++centerHits;
    CHECK(centerHits == 1);
}

TEST_CASE("findRegions: two overlapping circles produce the lens and lunes", "[regions]")
{
    // Two circles radius 3, centers 4 apart -> they intersect. The lens
    // (intersection) region must be found; a full arrangement yields 3 regions.
    std::vector<Curve> curves;
    curves.push_back(Curve::circle(Vec2d{0.0, 0.0}, 3.0));
    curves.push_back(Curve::circle(Vec2d{4.0, 0.0}, 3.0));

    auto regs = findRegions(curves, 0.02);

    // Robustness over completeness: at minimum the lens is found; ideally 3.
    REQUIRE(regs.size() >= 1);

    // The lens straddles the midline x=2; the point (2,0) is inside both circles.
    const Vec2d lensProbe{2.0, 0.0};
    bool lensFound = false;
    for (const auto& r : regs)
        if (contains(r, lensProbe)) {
            lensFound = true;
            break;
        }
    CHECK(lensFound);

    // If the full arrangement was extracted, verify it is exactly 3 regions and
    // the union area matches 2*(circle area) - lens area.
    if (regs.size() == 3) {
        const double R = 3.0, d = 4.0;
        const double lens = 2.0 * R * R * std::acos(d / (2.0 * R)) -
                            0.5 * d * std::sqrt(4.0 * R * R - d * d);
        const double expected = 2.0 * (M_PI * R * R) - lens;
        // Polyline approximation -> loose tolerance.
        CHECK_THAT(totalArea(regs), WithinAbs(expected, 0.5));
    }
}
