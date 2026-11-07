#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include "cmd/CommandProcessor.h"
#include "doc/Entities.h"
#include "doc/EntitiesEx.h"
#include "solid/SolidEntity.h"
#include "solid/SolidOps.h"

using namespace viki;
using Catch::Approx;

namespace {

double volumeOf(const TopoDS_Shape& s)
{
    GProp_GProps props;
    BRepGProp::VolumeProperties(s, props);
    return props.Mass();
}

// A closed axis-aligned square polyline of side `side` centred on `cx,cy`.
std::unique_ptr<PolylineEntity> square(double cx, double cy, double side)
{
    const double h = side / 2.0;
    std::vector<PolyVertex> vs = {
        {{cx - h, cy - h}, 0.0},
        {{cx + h, cy - h}, 0.0},
        {{cx + h, cy + h}, 0.0},
        {{cx - h, cy + h}, 0.0},
    };
    return std::make_unique<PolylineEntity>(std::move(vs), /*closed=*/true);
}

struct Rig {
    Document doc;
    SelectionSet selection;
    CommandContext ctx{doc, selection};
    CommandProcessor processor{ctx};
    Rig() { registerBuiltinCommands(processor); }
    bool run(const QString& s) { return processor.submit(s, true).ok; }

    EntityId add(std::unique_ptr<Entity> e)
    {
        doc.beginTransaction(QStringLiteral("add"));
        const EntityId id = doc.addEntity(std::move(e));
        doc.commitTransaction();
        return id;
    }
    int solidCount() const
    {
        int n = 0;
        for (const EntityId id : doc.drawOrder())
            if (dynamic_cast<const SolidEntity*>(doc.entity(id)))
                ++n;
        return n;
    }
    const SolidEntity* firstSolid() const
    {
        for (const EntityId id : doc.drawOrder())
            if (const auto* s = dynamic_cast<const SolidEntity*>(doc.entity(id)))
                return s;
        return nullptr;
    }
};

} // namespace

TEST_CASE("sweepProfile: circle radius 2 along a 50mm line ~= pi*r^2*L", "[sweep]")
{
    Document doc;
    // Profile: a radius-2 circle at the origin (on the XY plane).
    doc.beginTransaction(QStringLiteral("build"));
    const EntityId circId =
        doc.addEntity(std::make_unique<CircleEntity>(Vec2d{0, 0}, 2.0));
    // Path: a straight 50mm line along +X, starting at the profile centre.
    const EntityId pathId =
        doc.addEntity(std::make_unique<LineEntity>(Vec2d{0, 0}, Vec2d{50, 0}));
    doc.commitTransaction();

    const WorkPlane plane; // world XY
    const auto profiles = solidops::wiresFromEntities(doc, {circId}, plane);
    REQUIRE(profiles.ok);
    const auto path = solidops::pathWireFromEntities(doc, {pathId}, plane);
    REQUIRE(path.ok);
    REQUIRE(path.wires.size() == 1u);

    const auto solid = solidops::sweepProfile(profiles.wires, path.wires.front());
    REQUIRE(solid.ok);
    REQUIRE_FALSE(solid.shape.IsNull());

    const double expected = M_PI * 2.0 * 2.0 * 50.0; // ~628.32
    REQUIRE(volumeOf(solid.shape) == Approx(expected).epsilon(0.01));
}

TEST_CASE("SWEEP command builds a solid from profile + path", "[sweep]")
{
    Rig rig;
    const EntityId circId = rig.add(std::make_unique<CircleEntity>(Vec2d{0, 0}, 2.0));
    const EntityId pathId =
        rig.add(std::make_unique<LineEntity>(Vec2d{0, 0}, Vec2d{50, 0}));

    // Profile picked first (via selection), then the path entity.
    rig.selection.add(circId);
    REQUIRE(rig.run(QStringLiteral("SWEEP %1").arg(pathId)));
    REQUIRE(rig.solidCount() == 1);
    const auto* s = rig.firstSolid();
    REQUIRE(s != nullptr);
    const double expected = M_PI * 2.0 * 2.0 * 50.0;
    REQUIRE(volumeOf(s->shape()) == Approx(expected).epsilon(0.01));
}

TEST_CASE("issue13: SWEEP along a cornered polyline keeps every leg", "[issue13]")
{
    // A 25 mm leg, a right-angle corner, another 25 mm leg. BRepOffsetAPI_MakePipe
    // over the whole cornered spine builds a shape OCCT calls VALID and CLOSED
    // that holds only the FIRST leg — measured 153.938 mm3 out of 307.876
    // (exactly 50 %), reported as a plain success. sweepProfile now sweeps one
    // tangent-continuous leg at a time and fuses them.
    Rig rig;
    std::vector<PolyVertex> vs = {{{0, 0}, 0.0}, {{0, 25}, 0.0}, {{25, 25}, 0.0}};
    const EntityId circle = rig.add(std::make_unique<CircleEntity>(Vec2d{0, 0}, 1.4));
    const EntityId poly =
        rig.add(std::make_unique<PolylineEntity>(std::move(vs), /*closed=*/false));

    rig.selection.add(circle);
    REQUIRE(rig.run(QStringLiteral("SWEEP %1").arg(poly)));
    REQUIRE(rig.solidCount() == 1);
    const auto* s = rig.firstSolid();
    REQUIRE(s != nullptr);

    const double expected = M_PI * 1.4 * 1.4 * 50.0; // 307.876
    // A faithful sweep of a cornered spine falls a little short of pi*r^2*L
    // (the elbow is trimmed, not filled), but it must be nowhere near the
    // 50 % MakePipe used to return.
    const double v = volumeOf(s->shape());
    CHECK(v > 0.95 * expected);
    CHECK(v <= 1.01 * expected);

    // And the result is a genuinely closed solid, not a leg plus a stump.
    int shells = 0;
    bool closed = true;
    for (TopExp_Explorer e(s->shape(), TopAbs_SHELL); e.More(); e.Next()) {
        ++shells;
        closed = closed && BRep_Tool::IsClosed(e.Current());
    }
    CHECK(shells == 1);
    CHECK(closed);
}

TEST_CASE("issue13: a multi-corner staircase keeps every leg too", "[issue13]")
{
    // Four 12 mm legs, three right-angle corners — the reporter's many-segment
    // tube. Before the fix this measured 147.781 mm3 out of 295.561 (50 %).
    Rig rig;
    std::vector<PolyVertex> vs = {{{0, 0}, 0.0},
                                 {{0, 12}, 0.0},
                                 {{12, 12}, 0.0},
                                 {{12, 24}, 0.0},
                                 {{24, 24}, 0.0}};
    const EntityId circle = rig.add(std::make_unique<CircleEntity>(Vec2d{0, 0}, 1.4));
    const EntityId poly =
        rig.add(std::make_unique<PolylineEntity>(std::move(vs), /*closed=*/false));

    rig.selection.add(circle);
    REQUIRE(rig.run(QStringLiteral("SWEEP %1").arg(poly)));
    REQUIRE(rig.solidCount() == 1);
    const auto* s = rig.firstSolid();
    REQUIRE(s != nullptr);

    const double expected = M_PI * 1.4 * 1.4 * 48.0; // 295.56
    const double v = volumeOf(s->shape());
    CHECK(v > 0.93 * expected);
    CHECK(v <= 1.01 * expected);
}

TEST_CASE("issue13: a curved (tangent-continuous) spine is untouched", "[issue13]")
{
    // The fix must not disturb the shape that always worked: a single arc is
    // ONE leg, so it still takes exactly one MakePipe over the whole spine.
    Rig rig;
    const EntityId circle = rig.add(std::make_unique<CircleEntity>(Vec2d{0, 0}, 1.4));
    // Quarter circle centred at (25,0): (0,0) at 180 deg sweeping to (25,25) at 90 deg.
    const EntityId arc = rig.add(std::make_unique<ArcEntity>(
        Vec2d{25, 0}, 25.0, M_PI, -M_PI / 2.0));
    rig.selection.add(circle);
    const auto r = rig.processor.submit(QStringLiteral("SWEEP %1").arg(arc), false);
    REQUIRE(rig.solidCount() == 1);
    const auto* s = rig.firstSolid();
    REQUIRE(s != nullptr);
    // Nominal for a quarter circle of radius 25: pi*r^2 * (pi*25/2).
    const double expected = M_PI * 1.4 * 1.4 * (M_PI * 25.0 / 2.0);
    CHECK(volumeOf(s->shape()) == Approx(expected).epsilon(0.02));
    (void)r;
}

TEST_CASE("loftProfiles: two concentric squares (10 at z0, 6 at z20)", "[loft]")
{
    Document doc;
    // Sections drawn on two parallel work planes 20mm apart in Z.
    doc.beginTransaction(QStringLiteral("build"));
    const EntityId sq10 = doc.addEntity(square(0, 0, 10.0)); // on z0 plane
    const EntityId sq6 = doc.addEntity(square(0, 0, 6.0));   // on z20 plane
    doc.commitTransaction();

    const WorkPlane planeZ0; // world XY at Z=0
    WorkPlane planeZ20;      // parallel plane offset to Z=20
    planeZ20.origin = gp_Pnt(0, 0, 20);

    const auto w0 = solidops::wiresFromEntities(doc, {sq10}, planeZ0);
    const auto w1 = solidops::wiresFromEntities(doc, {sq6}, planeZ20);
    REQUIRE(w0.ok);
    REQUIRE(w1.ok);

    std::vector<TopoDS_Wire> sections = {w0.wires.front(), w1.wires.front()};
    const auto solid = solidops::loftProfiles(sections, /*solid=*/true);
    REQUIRE(solid.ok);
    REQUIRE_FALSE(solid.shape.IsNull());

    // Volume sits strictly between the two extreme prisms:
    //   small: 6x6x20 = 720 ; large: 10x10x20 = 2000.
    const double v = volumeOf(solid.shape);
    REQUIRE(v > 720.0);
    REQUIRE(v < 2000.0);
}

TEST_CASE("loftProfiles: needs at least two sections", "[loft]")
{
    Document doc;
    doc.beginTransaction(QStringLiteral("build"));
    const EntityId sq = doc.addEntity(square(0, 0, 10.0));
    doc.commitTransaction();
    const auto w = solidops::wiresFromEntities(doc, {sq}, WorkPlane{});
    REQUIRE(w.ok);
    const auto solid = solidops::loftProfiles({w.wires.front()}, true);
    REQUIRE_FALSE(solid.ok);
}
