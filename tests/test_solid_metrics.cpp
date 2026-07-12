#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <TopoDS_Shape.hxx>

#include "solid/SolidMetrics.h"

// solidops::solidMetrics — the shared "quick numbers" helper behind
// DESCRIBE / query describe AND the LIST solid line. One computation,
// two renderings, so this unit test covers both call sites' numbers.

using namespace viki;
using Catch::Approx;

TEST_CASE("solidMetrics of a known box", "[solid][metrics]")
{
    // 40 x 30 x 10 box at the origin. Don't trust IsDone() on BRepPrimAPI
    // builders (LESSONS.md) — take .Shape() and null-check.
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(40.0, 30.0, 10.0).Shape();
    REQUIRE_FALSE(box.IsNull());

    const auto m = solidops::solidMetrics(box);
    REQUIRE(m.valid);
    CHECK(m.volume == Approx(40.0 * 30.0 * 10.0).epsilon(1e-6));
    // 2*(40*30 + 40*10 + 30*10) = 3800 mm2
    CHECK(m.area == Approx(3800.0).epsilon(1e-6));
    // Bnd_Box carries a ~1e-7 fringe: margin, not epsilon.
    CHECK(m.bboxMin.X() == Approx(0.0).margin(1e-4));
    CHECK(m.bboxMin.Y() == Approx(0.0).margin(1e-4));
    CHECK(m.bboxMin.Z() == Approx(0.0).margin(1e-4));
    CHECK(m.bboxMax.X() == Approx(40.0).margin(1e-4));
    CHECK(m.bboxMax.Y() == Approx(30.0).margin(1e-4));
    CHECK(m.bboxMax.Z() == Approx(10.0).margin(1e-4));
    CHECK(m.centroid.X() == Approx(20.0).margin(1e-6));
    CHECK(m.centroid.Y() == Approx(15.0).margin(1e-6));
    CHECK(m.centroid.Z() == Approx(5.0).margin(1e-6));
}

TEST_CASE("solidMetrics of a cylinder", "[solid][metrics]")
{
    // r=5, h=20: volume = pi*25*20, area = 2*pi*25 + 2*pi*5*20.
    const TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(5.0, 20.0).Shape();
    REQUIRE_FALSE(cyl.IsNull());

    const auto m = solidops::solidMetrics(cyl);
    REQUIRE(m.valid);
    const double pi = 3.14159265358979323846;
    CHECK(m.volume == Approx(pi * 25.0 * 20.0).epsilon(1e-6));
    CHECK(m.area == Approx(2 * pi * 25.0 + 2 * pi * 5.0 * 20.0).epsilon(1e-6));
    CHECK(m.bboxMin.Z() == Approx(0.0).margin(1e-4));
    CHECK(m.bboxMax.Z() == Approx(20.0).margin(1e-4));
    CHECK(m.centroid.Z() == Approx(10.0).margin(1e-6));
}

TEST_CASE("solidMetrics of a null shape is invalid and all-zero",
          "[solid][metrics]")
{
    const auto m = solidops::solidMetrics(TopoDS_Shape());
    CHECK_FALSE(m.valid);
    CHECK(m.volume == 0.0);
    CHECK(m.area == 0.0);
}
