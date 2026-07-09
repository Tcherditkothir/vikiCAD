#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Dir.hxx>
#include <gp_Vec.hxx>

#include "render/StandardViews.h"
#include "solid/SolidOps.h"

using namespace viki;
using namespace viki::views;

namespace {
// True if two unit directions point the same way (within tolerance).
bool sameDir(const gp_Dir& a, double x, double y, double z)
{
    return std::abs(a.X() - x) < 1e-9 && std::abs(a.Y() - y) < 1e-9 &&
           std::abs(a.Z() - z) < 1e-9;
}
} // namespace

TEST_CASE("standardViewDir gives the six orthographic cameras", "[views]")
{
    // TOP: look straight down (-Z) with +Y up.
    auto top = standardViewDir("TOP");
    REQUIRE(top.has_value());
    CHECK(sameDir(top->dir, 0, 0, -1));
    CHECK(sameDir(top->up, 0, 1, 0));

    // FRONT: look along +Y with +Z up.
    auto front = standardViewDir("FRONT");
    REQUIRE(front.has_value());
    CHECK(sameDir(front->dir, 0, 1, 0));
    CHECK(sameDir(front->up, 0, 0, 1));

    // BACK: opposite look, still +Z up.
    auto back = standardViewDir("BACK");
    REQUIRE(back.has_value());
    CHECK(sameDir(back->dir, 0, -1, 0));
    CHECK(sameDir(back->up, 0, 0, 1));

    // BOTTOM: look up (+Z).
    auto bottom = standardViewDir("BOTTOM");
    REQUIRE(bottom.has_value());
    CHECK(sameDir(bottom->dir, 0, 0, 1));

    // LEFT / RIGHT look along ±X with +Z up.
    auto left = standardViewDir("LEFT");
    auto right = standardViewDir("RIGHT");
    REQUIRE(left.has_value());
    REQUIRE(right.has_value());
    CHECK(sameDir(left->dir, 1, 0, 0));
    CHECK(sameDir(right->dir, -1, 0, 0));
    CHECK(sameDir(left->up, 0, 0, 1));
    CHECK(sameDir(right->up, 0, 0, 1));
}

TEST_CASE("standardViewDir is case-insensitive and validates names", "[views]")
{
    CHECK(standardViewDir("top").has_value());
    CHECK(standardViewDir(" Front ").has_value());
    CHECK_FALSE(standardViewDir("SIDEWAYS").has_value());
    CHECK_FALSE(standardViewDir("").has_value());
}

TEST_CASE("standardViewDir ISO gives a valid orthonormal camera", "[views]")
{
    auto iso = standardViewDir("ISO");
    REQUIRE(iso.has_value());
    // NE isometric looks down toward the origin from +X/+Y/+Z-ish.
    CHECK(iso->dir.X() > 0);
    CHECK(iso->dir.Y() > 0);
    CHECK(iso->dir.Z() < 0);
    // dir and up must be orthogonal unit vectors.
    CHECK(std::abs(gp_Vec(iso->dir).Dot(gp_Vec(iso->up))) < 1e-9);
    // +Z projects upward on screen (up has a positive Z component).
    CHECK(iso->up.Z() > 0);
    CHECK(standardViewDir("ISOMETRIC").has_value());
}

TEST_CASE("alignToFaceDir looks along a face normal", "[views]")
{
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();

    bool checkedTop = false;
    for (TopExp_Explorer e(box, TopAbs_FACE); e.More(); e.Next()) {
        auto ori = alignToFaceDir(e.Current());
        REQUIRE(ori.has_value());
        // dir and up are orthonormal.
        CHECK(std::abs(gp_Vec(ori->dir).Dot(gp_Vec(ori->up))) < 1e-9);

        // Identify the +Z (top) face via planeFromFace and verify the camera
        // looks straight down onto it (-Z view direction).
        auto wp = solidops::planeFromFace(e.Current());
        REQUIRE(wp.has_value());
        if (std::abs(wp->normal.Z() - 1.0) < 1e-6) {
            checkedTop = true;
            CHECK(sameDir(ori->dir, 0, 0, -1)); // look opposite the +Z normal
        }
    }
    CHECK(checkedTop);
}

TEST_CASE("alignToFaceDir rejects non-planar / null shapes", "[views]")
{
    CHECK_FALSE(alignToFaceDir(TopoDS_Shape()).has_value());
    // A whole box (a solid, not a face) has no single normal.
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape();
    CHECK_FALSE(alignToFaceDir(box).has_value());
}
