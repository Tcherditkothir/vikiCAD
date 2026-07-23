#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

#include "doc/Document.h"
#include "doc/RectProfile.h"

// Rectangle recognition + Length/Height editing (the Properties panel's
// "Rectangle" rows — usage fix 2026-07-23). All expected vertices below are
// DERIVED BY HAND. Conventions under test (documented in RectProfile.h):
// anchor = vertex closest to the world origin (ties -> lowest index), Length
// = the LONGER side, edits stretch along the rectangle's OWN original axes.

using namespace viki;
using Catch::Approx;

namespace {

PolylineEntity makePoly(std::initializer_list<Vec2d> pts, bool closed = true,
                        std::initializer_list<double> bulges = {})
{
    std::vector<PolyVertex> vs;
    size_t i = 0;
    std::vector<double> bl(bulges);
    for (const Vec2d& p : pts) {
        vs.push_back({p, i < bl.size() ? bl[i] : 0.0});
        ++i;
    }
    return PolylineEntity(std::move(vs), closed);
}

void checkVert(const PolyVertex& v, double x, double y)
{
    CHECK(v.pos.x == Approx(x).margin(1e-9));
    CHECK(v.pos.y == Approx(y).margin(1e-9));
    CHECK(v.bulge == 0.0);
}

} // namespace

TEST_CASE("rectangle detection: true and false cases", "[rectprofile]")
{
    SECTION("axis-aligned RECT output is a rectangle")
    {
        const auto pl = makePoly({{5, 5}, {35, 5}, {35, 25}, {5, 25}});
        const auto r = rectProfileOf(pl);
        REQUIRE(r.has_value());
        CHECK(r->anchorIndex == 0); // (5,5) is closest to the origin
        CHECK(r->length() == Approx(30.0));
        CHECK(r->height() == Approx(20.0));
        CHECK(r->area() == Approx(600.0));
    }

    SECTION("rotated 30 degrees still detected, own axes reported")
    {
        const double c30 = std::sqrt(3.0) / 2.0;
        const Vec2d u{c30, 0.5};   // long axis, 30 deg
        const Vec2d w{-0.5, c30};  // short axis
        const Vec2d v0{2, 1};
        const auto pl =
            makePoly({v0, v0 + u * 10.0, v0 + u * 10.0 + w * 4.0, v0 + w * 4.0});
        const auto r = rectProfileOf(pl);
        REQUIRE(r.has_value());
        CHECK(r->anchorIndex == 0); // |v0| = sqrt(5), smallest of the four
        CHECK(r->length() == Approx(10.0));
        CHECK(r->height() == Approx(4.0));
        CHECK(r->dirFwd.x == Approx(c30).margin(1e-12));
        CHECK(r->dirFwd.y == Approx(0.5).margin(1e-12));
    }

    SECTION("a bulged side is not a rectangle")
    {
        const auto pl =
            makePoly({{0, 0}, {10, 0}, {10, 5}, {0, 5}}, true, {0.5, 0, 0, 0});
        CHECK_FALSE(rectProfileOf(pl).has_value());
    }

    SECTION("five vertices are not a rectangle (even with square corners)")
    {
        const auto pl = makePoly({{0, 0}, {5, 0}, {10, 0}, {10, 5}, {0, 5}});
        CHECK_FALSE(rectProfileOf(pl).has_value());
    }

    SECTION("an open polyline is not a rectangle")
    {
        const auto pl = makePoly({{0, 0}, {10, 0}, {10, 5}, {0, 5}}, false);
        CHECK_FALSE(rectProfileOf(pl).has_value());
    }

    SECTION("a parallelogram is not a rectangle")
    {
        const auto pl = makePoly({{0, 0}, {10, 0}, {12, 5}, {2, 5}});
        CHECK_FALSE(rectProfileOf(pl).has_value());
    }
}

TEST_CASE("rectangle edit: axis-aligned, anchored, by hand", "[rectprofile]")
{
    auto pl = makePoly({{5, 5}, {35, 5}, {35, 25}, {5, 25}});

    SECTION("length 30 -> 40 keeps the (5,5) anchor")
    {
        REQUIRE(applyRectDims(pl, 40.0, 20.0));
        checkVert(pl.vertices()[0], 5, 5);
        checkVert(pl.vertices()[1], 45, 5);
        checkVert(pl.vertices()[2], 45, 25);
        checkVert(pl.vertices()[3], 5, 25);
    }

    SECTION("height 20 -> 8")
    {
        REQUIRE(applyRectDims(pl, 30.0, 8.0));
        checkVert(pl.vertices()[0], 5, 5);
        checkVert(pl.vertices()[1], 35, 5);
        checkVert(pl.vertices()[2], 35, 13);
        checkVert(pl.vertices()[3], 5, 13);
    }

    SECTION("non-positive dimensions are refused, vertices untouched")
    {
        CHECK_FALSE(applyRectDims(pl, 0.0, 20.0));
        CHECK_FALSE(applyRectDims(pl, 30.0, -5.0));
        checkVert(pl.vertices()[1], 35, 5);
    }
}

TEST_CASE("rectangle edit: anchor away from vertex 0 (negative quadrant)",
          "[rectprofile]")
{
    // Closest vertex to the origin is index 2 = (-10,-5): |.|^2 = 125 vs
    // 1300 / 500 / 925 for the others.
    auto pl = makePoly({{-30, -20}, {-10, -20}, {-10, -5}, {-30, -5}});
    const auto r = rectProfileOf(pl);
    REQUIRE(r.has_value());
    CHECK(r->anchorIndex == 2);
    CHECK(r->length() == Approx(20.0)); // long side along -x from the anchor
    CHECK(r->height() == Approx(15.0));

    // 20 x 15 -> 30 x 10: anchor (-10,-5) fixed, sides grow along the
    // ORIGINAL directions: fwd = toward v3 = (-1,0), back = toward v1 =
    // (0,-1). By hand: v3=(-40,-5), v0=(-40,-15), v1=(-10,-15).
    REQUIRE(applyRectDims(pl, 30.0, 10.0));
    checkVert(pl.vertices()[0], -40, -15);
    checkVert(pl.vertices()[1], -10, -15);
    checkVert(pl.vertices()[2], -10, -5);
    checkVert(pl.vertices()[3], -40, -5);
}

TEST_CASE("rectangle edit: rotated rectangle keeps its own axes",
          "[rectprofile]")
{
    const double c30 = std::sqrt(3.0) / 2.0;
    const Vec2d u{c30, 0.5};
    const Vec2d w{-0.5, c30};
    const Vec2d v0{2, 1};
    auto pl =
        makePoly({v0, v0 + u * 10.0, v0 + u * 10.0 + w * 4.0, v0 + w * 4.0});

    // L 10 -> 12, H 4 -> 6. By hand:
    //   v1 = v0 + 12u = (2 + 12*c30, 1 + 6)      = (12.392304845413264, 7)
    //   v2 = v1 + 6w  = (v1.x - 3, v1.y + 6*c30) = (9.392304845413264,
    //                                              12.196152422706632)
    //   v3 = v0 + 6w  = (2 - 3, 1 + 6*c30)       = (-1, 6.196152422706632)
    REQUIRE(applyRectDims(pl, 12.0, 6.0));
    checkVert(pl.vertices()[0], 2, 1);
    checkVert(pl.vertices()[1], 2 + 12 * c30, 7);
    checkVert(pl.vertices()[2], 2 + 12 * c30 - 3, 7 + 6 * c30);
    checkVert(pl.vertices()[3], -1, 1 + 6 * c30);

    // Still a rectangle afterwards, same orientation, new dimensions.
    const auto r = rectProfileOf(pl);
    REQUIRE(r.has_value());
    CHECK(r->length() == Approx(12.0));
    CHECK(r->height() == Approx(6.0));
    CHECK(r->dirFwd.x == Approx(c30).margin(1e-12));
}

TEST_CASE("rectangle edit is undoable through the document journal",
          "[rectprofile]")
{
    Document doc;
    auto owned = std::make_unique<PolylineEntity>();
    *owned = makePoly({{5, 5}, {35, 5}, {35, 25}, {5, 25}});
    doc.beginTransaction(QStringLiteral("RECT"));
    const EntityId id = doc.addEntity(std::move(owned));
    doc.commitTransaction();

    // The panel's exact mutation shape: beginModify -> applyRectDims ->
    // endModify inside one transaction.
    doc.beginTransaction(QStringLiteral("RECT EDIT"));
    {
        auto* pl = dynamic_cast<PolylineEntity*>(doc.beginModify(id));
        REQUIRE(pl != nullptr);
        REQUIRE(applyRectDims(*pl, 40.0, 8.0));
        doc.endModify(id);
    }
    doc.commitTransaction();
    {
        const auto* pl = dynamic_cast<const PolylineEntity*>(doc.entity(id));
        REQUIRE(pl != nullptr);
        checkVert(pl->vertices()[2], 45, 13);
    }

    REQUIRE(doc.undo() == QStringLiteral("RECT EDIT"));
    {
        const auto* pl = dynamic_cast<const PolylineEntity*>(doc.entity(id));
        REQUIRE(pl != nullptr);
        checkVert(pl->vertices()[0], 5, 5);
        checkVert(pl->vertices()[1], 35, 5);
        checkVert(pl->vertices()[2], 35, 25);
        checkVert(pl->vertices()[3], 5, 25);
    }

    REQUIRE(doc.redo() == QStringLiteral("RECT EDIT"));
    {
        const auto* pl = dynamic_cast<const PolylineEntity*>(doc.entity(id));
        REQUIRE(pl != nullptr);
        checkVert(pl->vertices()[2], 45, 13);
    }
}
