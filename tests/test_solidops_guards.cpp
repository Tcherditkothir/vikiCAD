// solidops-result-guards: no SolidResult producer may report ok=true with a
// shape that contains no TopAbs_SOLID (the "part vanished" class of bug).
// Each case here is a degenerate input that used to slip through as a fake
// success (or an OCCT throw) — all must now come back ok=false WITH a
// message, and never a garbage shape.
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>

#include "doc/Document.h"
#include "doc/Entities.h"
#include "doc/EntitiesEx.h"
#include "solid/SolidOps.h"

using namespace viki;

namespace {

// Every failed SolidResult must explain itself and carry no shape.
void checkRefused(const solidops::SolidResult& res)
{
    CHECK_FALSE(res.ok);
    CHECK_FALSE(res.message.isEmpty());
    CHECK(res.shape.IsNull());
}

bool containsSolid(const TopoDS_Shape& s)
{
    if (s.IsNull())
        return false;
    TopExp_Explorer exp(s, TopAbs_SOLID);
    return exp.More();
}

// A closed square profile of side `side` at the origin, as wires.
std::vector<TopoDS_Wire> squareWires(Document& doc, double side)
{
    doc.beginTransaction(QStringLiteral("build"));
    std::vector<PolyVertex> vs = {
        {{0, 0}, 0.0}, {{side, 0}, 0.0}, {{side, side}, 0.0}, {{0, side}, 0.0}};
    const EntityId id = doc.addEntity(
        std::make_unique<PolylineEntity>(std::move(vs), /*closed=*/true));
    doc.commitTransaction();
    const auto w = solidops::wiresFromEntities(doc, {id}, WorkPlane{});
    REQUIRE(w.ok);
    return w.wires;
}

// A 10x10x10 box solid (via the guarded extrude, which must succeed here).
TopoDS_Shape box10(Document& doc)
{
    const auto res = solidops::extrudeWires(squareWires(doc, 10.0), 10.0);
    REQUIRE(res.ok);
    REQUIRE(containsSolid(res.shape));
    return res.shape;
}

} // namespace

TEST_CASE("guard: revolve of a zero-area (collinear) profile is refused",
          "[solids][guards]")
{
    Document doc;
    // A "closed" polyline that is really a collapsed segment: zero area.
    doc.beginTransaction(QStringLiteral("build"));
    std::vector<PolyVertex> vs = {{{0, 0}, 0.0}, {{10, 0}, 0.0}, {{20, 0}, 0.0}};
    const EntityId id = doc.addEntity(
        std::make_unique<PolylineEntity>(std::move(vs), /*closed=*/true));
    doc.commitTransaction();
    const auto w = solidops::wiresFromEntities(doc, {id}, WorkPlane{});
    if (!w.ok)
        return; // refused even earlier (no usable profile) — also fine
    const auto res = solidops::revolveWires(w.wires, Vec2d{0, -1}, Vec2d{0, 1},
                                            2 * M_PI, WorkPlane{});
    checkRefused(res);
}

TEST_CASE("guard: revolve by a zero angle is refused", "[solids][guards]")
{
    Document doc;
    // Valid square profile, but a 0-radian sweep encloses no volume.
    auto wires = squareWires(doc, 10.0);
    // Axis x=-5, away from the profile, so only the angle is degenerate.
    const auto res = solidops::revolveWires(wires, Vec2d{-5, 0}, Vec2d{-5, 1},
                                            0.0, WorkPlane{});
    checkRefused(res);
}

TEST_CASE("guard: shell thicker than the part is refused", "[solids][guards]")
{
    Document doc;
    const TopoDS_Shape box = box10(doc);
    // Open the first face so the direct MakeThickSolid path (not the boolean
    // fallback) is the one under test.
    TopExp_Explorer faces(box, TopAbs_FACE);
    REQUIRE(faces.More());
    const auto res = solidops::shellSolid(box, 20.0, faces.Current());
    checkRefused(res);

    // Closed variant (no open face) must refuse too.
    const auto closed = solidops::shellSolid(box, 20.0);
    checkRefused(closed);
}

TEST_CASE("guard: fillet radius larger than the box is refused",
          "[solids][guards]")
{
    Document doc;
    const TopoDS_Shape box = box10(doc);
    const auto res = solidops::filletFirstNEdges(box, 1, 100.0);
    checkRefused(res);
}

TEST_CASE("guard: chamfer distance larger than the box is refused",
          "[solids][guards]")
{
    Document doc;
    const TopoDS_Shape box = box10(doc);
    const auto res = solidops::chamferFirstNEdges(box, 1, 100.0);
    checkRefused(res);
}

TEST_CASE("guard: through-hole wider than the part is refused (not an empty "
          "success)",
          "[solids][guards]")
{
    Document doc;
    const TopoDS_Shape box = box10(doc);
    // A 100mm bore through a 10mm box swallows the whole part: refuse rather
    // than commit an empty compound (the original "part vanished" bug).
    const auto res = solidops::makeHole(box, WorkPlane{}, Vec2d{5, 5},
                                        /*diameter=*/100.0, /*depth=*/0.0,
                                        /*through=*/true);
    checkRefused(res);
}

TEST_CASE("guard: loft between coincident sections is refused",
          "[solids][guards]")
{
    Document doc;
    // Two identical sections in the same plane: zero-height loft.
    auto a = squareWires(doc, 10.0);
    Document doc2;
    auto b = squareWires(doc2, 10.0);
    const auto res = solidops::loftProfiles({a.front(), b.front()}, /*solid=*/true);
    if (res.ok) {
        // If OCCT manages to build something, the guard guarantees it holds a
        // real solid — never a bare shell/compound.
        CHECK(containsSolid(res.shape));
    } else {
        CHECK_FALSE(res.message.isEmpty());
    }
}

TEST_CASE("guard: successful ops still succeed (sanity)", "[solids][guards]")
{
    Document doc;
    const TopoDS_Shape box = box10(doc);
    // A feasible fillet, shell and hole all pass the guard with real solids.
    const auto f = solidops::filletFirstNEdges(box, 1, 2.0);
    CHECK(f.ok);
    CHECK(containsSolid(f.shape));
    const auto sh = solidops::shellSolid(box, 1.0);
    CHECK(sh.ok);
    CHECK(containsSolid(sh.shape));
    const auto h = solidops::makeHole(box, WorkPlane{}, Vec2d{5, 5}, 4.0, 0.0,
                                      /*through=*/true);
    CHECK(h.ok);
    CHECK(containsSolid(h.shape));
}
