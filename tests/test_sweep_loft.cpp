#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
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
