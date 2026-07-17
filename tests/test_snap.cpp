#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

#include "doc/Annotations.h"
#include "doc/Block.h"
#include "doc/Entities.h"
#include "doc/EntitiesEx.h"
#include "snap/SnapEngine.h"

using namespace viki;
using Catch::Approx;

namespace {
Document makeDoc()
{
    Document doc;
    doc.beginTransaction(QStringLiteral("setup"));
    doc.addEntity(std::make_unique<LineEntity>(Vec2d{0, 0}, Vec2d{100, 0}));   // id 1
    doc.addEntity(std::make_unique<LineEntity>(Vec2d{50, -50}, Vec2d{50, 50})); // id 2
    doc.addEntity(std::make_unique<CircleEntity>(Vec2d{200, 0}, 25));           // id 3
    doc.commitTransaction();
    return doc;
}
} // namespace

TEST_CASE("endpoint beats midpoint and nearest wins", "[snap]")
{
    const Document doc = makeDoc();
    SnapSettings s;
    const auto r = snapQuery(doc, {99, 1}, 5.0, s, std::nullopt);
    REQUIRE(r);
    REQUIRE(r->kind == SnapKind::Endpoint);
    REQUIRE(r->point.x == Approx(100.0));
}

TEST_CASE("midpoint snap", "[snap]")
{
    const Document doc = makeDoc();
    SnapSettings s;
    const auto r = snapQuery(doc, {49, 2}, 5.0, s, std::nullopt);
    REQUIRE(r);
    // (50,0) is both the midpoint of line 1 and the intersection of 1 and 2 —
    // intersection outranks midpoint.
    REQUIRE(r->kind == SnapKind::Intersection);
    REQUIRE(r->point.x == Approx(50.0));
    REQUIRE(r->point.y == Approx(0.0).margin(1e-9));
}

TEST_CASE("center and quadrant snaps on circle", "[snap]")
{
    const Document doc = makeDoc();
    SnapSettings s;
    const auto center = snapQuery(doc, {201, 1}, 5.0, s, std::nullopt);
    REQUIRE(center);
    REQUIRE(center->kind == SnapKind::Center);

    const auto quad = snapQuery(doc, {224, 1}, 5.0, s, std::nullopt);
    REQUIRE(quad);
    REQUIRE(quad->kind == SnapKind::Quadrant);
    REQUIRE(quad->point.x == Approx(225.0));
}

TEST_CASE("perpendicular snap from a base point", "[snap]")
{
    const Document doc = makeDoc();
    SnapSettings s;
    s.endpoint = s.midpoint = s.center = s.quadrant = s.intersection = false;
    // Base at (30,40): perpendicular foot on line 1 (the X axis) is (30,0).
    const auto r = snapQuery(doc, {29, 1}, 5.0, s, Vec2d{30, 40});
    REQUIRE(r);
    REQUIRE(r->kind == SnapKind::Perpendicular);
    REQUIRE(r->point.x == Approx(30.0));
    REQUIRE(r->point.y == Approx(0.0).margin(1e-9));
}

TEST_CASE("nearest snap returns the foot of perpendicular on a line", "[snap]")
{
    const Document doc = makeDoc();
    SnapSettings s;
    // Isolate NEAREST: turn off every other typed mode so only the closest point
    // on an entity competes.
    s.endpoint = s.node = s.midpoint = s.center = s.quadrant = false;
    s.intersection = s.perpendicular = s.tangent = false;
    // Line 1 is the X axis from (0,0) to (100,0). Cursor above it at (30,3): the
    // closest point on the line is the foot of the perpendicular, (30,0).
    const auto r = snapQuery(doc, {30, 3}, 5.0, s, std::nullopt);
    REQUIRE(r);
    REQUIRE(r->kind == SnapKind::Nearest);
    REQUIRE(r->point.x == Approx(30.0));
    REQUIRE(r->point.y == Approx(0.0).margin(1e-9));
}

TEST_CASE("node snap targets a POINT entity", "[snap]")
{
    Document doc;
    doc.beginTransaction(QStringLiteral("setup"));
    doc.addEntity(std::make_unique<PointEntity>(Vec2d{40, 60}));
    doc.commitTransaction();

    SnapSettings s;
    const auto r = snapQuery(doc, {41, 59}, 5.0, s, std::nullopt);
    REQUIRE(r);
    REQUIRE(r->kind == SnapKind::Node);
    REQUIRE(r->point.x == Approx(40.0));
    REQUIRE(r->point.y == Approx(60.0));

    // Toggleable: disabling node drops the candidate.
    s.node = false;
    s.nearest = false; // a POINT has no segments, but be explicit
    REQUIRE_FALSE(snapQuery(doc, {41, 59}, 5.0, s, std::nullopt));
}

TEST_CASE("tangent snap from an external point to a circle", "[snap]")
{
    Document doc;
    doc.beginTransaction(QStringLiteral("setup"));
    doc.addEntity(std::make_unique<CircleEntity>(Vec2d{0, 0}, 30.0));
    doc.commitTransaction();

    SnapSettings s;
    // Only tangent is interesting here; keep the higher-priority modes off so
    // the tangent point is what gets returned near the cursor.
    s.endpoint = s.node = s.midpoint = s.center = s.quadrant = false;
    s.intersection = s.perpendicular = s.nearest = false;

    // Base well outside the circle; one tangent point lies up-and-right.
    const Vec2d base{100, 0};
    // Cursor near the expected upper tangent point of a circle r=30, base at
    // (100,0): tangent length L=sqrt(100^2-30^2), touch point roughly (9,28.6).
    const auto r = snapQuery(doc, {9, 29}, 6.0, s, base);
    REQUIRE(r);
    REQUIRE(r->kind == SnapKind::Tangent);

    const Vec2d tp = r->point;
    // The tangent point lies on the circle.
    REQUIRE(tp.length() == Approx(30.0).margin(1e-9));
    // Radius (center->tp) is perpendicular to the tangent line (base->tp):
    // their dot product is ~0.
    const Vec2d radius = tp;              // center is origin
    const Vec2d tangentLine = tp - base;  // along the tangent
    REQUIRE(radius.dot(tangentLine) == Approx(0.0).margin(1e-6));
}

TEST_CASE("tangent needs an external base (none inside the circle)", "[snap]")
{
    Document doc;
    doc.beginTransaction(QStringLiteral("setup"));
    doc.addEntity(std::make_unique<CircleEntity>(Vec2d{0, 0}, 30.0));
    doc.commitTransaction();

    SnapSettings s;
    s.endpoint = s.node = s.midpoint = s.center = s.quadrant = false;
    s.intersection = s.perpendicular = s.nearest = false;
    // Base inside the circle: no real tangent exists.
    REQUIRE_FALSE(snapQuery(doc, {10, 5}, 6.0, s, Vec2d{5, 0}));
}

TEST_CASE("disabled master switch returns nothing", "[snap]")
{
    const Document doc = makeDoc();
    SnapSettings s;
    s.enabled = false;
    REQUIRE_FALSE(snapQuery(doc, {0, 0}, 5.0, s, std::nullopt));
}

TEST_CASE("snap ignores invisible layers", "[snap]")
{
    Document doc = makeDoc();
    const LayerId hidden = doc.ensureLayer(QStringLiteral("hidden"), 0xFFFFFF, false);
    doc.beginTransaction(QStringLiteral("h"));
    auto line = std::make_unique<LineEntity>(Vec2d{300, 0}, Vec2d{400, 0});
    line->setLayerId(hidden);
    doc.addEntity(std::move(line));
    doc.commitTransaction();
    // The entity kept its explicit layer (hidden), so no snap there.
    SnapSettings s;
    REQUIRE_FALSE(snapQuery(doc, {300, 0}, 5.0, s, std::nullopt));
}

TEST_CASE("snap reaches inside block inserts", "[snap][block]")
{
    Document doc;
    BlockDef* def = doc.createBlock(QStringLiteral("B"), {0, 0});
    def->entities.push_back(
        std::make_unique<LineEntity>(Vec2d{0, 0}, Vec2d{10, 0}));

    doc.beginTransaction(QStringLiteral("setup"));
    auto ins = std::make_unique<InsertEntity>();
    ins->blockName = QStringLiteral("B");
    ins->position = {100, 100};
    ins->rotation = M_PI / 2; // scaled 2x: line runs (100,100) -> (100,120)
    ins->scale = 2.0;
    doc.addEntity(std::move(ins));
    doc.commitTransaction();

    SnapSettings s;
    const auto end = snapQuery(doc, {101, 119}, 5.0, s, std::nullopt);
    REQUIRE(end);
    REQUIRE(end->kind == SnapKind::Endpoint);
    REQUIRE(end->point.x == Approx(100.0).margin(1e-9));
    REQUIRE(end->point.y == Approx(120.0));

    const auto mid = snapQuery(doc, {99, 110}, 5.0, s, std::nullopt);
    REQUIRE(mid);
    REQUIRE(mid->kind == SnapKind::Midpoint);
    REQUIRE(mid->point.x == Approx(100.0).margin(1e-9));
    REQUIRE(mid->point.y == Approx(110.0));
}

TEST_CASE("snap reaches nested block inserts", "[snap][block]")
{
    Document doc;
    BlockDef* inner = doc.createBlock(QStringLiteral("INNER"), {0, 0});
    inner->entities.push_back(
        std::make_unique<CircleEntity>(Vec2d{5, 0}, 2));

    BlockDef* outer = doc.createBlock(QStringLiteral("OUTER"), {0, 0});
    {
        auto sub = std::make_unique<InsertEntity>();
        sub->blockName = QStringLiteral("INNER");
        sub->position = {20, 0};
        outer->entities.push_back(std::move(sub));
    }

    doc.beginTransaction(QStringLiteral("setup"));
    auto ins = std::make_unique<InsertEntity>();
    ins->blockName = QStringLiteral("OUTER");
    ins->position = {50, 50};
    doc.addEntity(std::move(ins));
    doc.commitTransaction();

    // Circle center ends up at (50+20+5, 50) = (75, 50).
    SnapSettings s;
    const auto center = snapQuery(doc, {76, 49}, 5.0, s, std::nullopt);
    REQUIRE(center);
    REQUIRE(center->kind == SnapKind::Center);
    REQUIRE(center->point.x == Approx(75.0));
    REQUIRE(center->point.y == Approx(50.0));
}

// --- Gerber measurement snaps (G2): pad centers and wide traces --------------

TEST_CASE("pad insert offers its flash origin as a Center snap", "[snap][gerber]")
{
    Document doc;
    // A GBR-style pad block: one SOLID hatch ring around the flash origin.
    BlockDef* def = doc.createBlock(QStringLiteral("GBR-D10"), {0, 0});
    {
        auto pad = std::make_unique<HatchEntity>();
        pad->rings.push_back({{-1, -0.5}, {1, -0.5}, {1, 0.5}, {-1, 0.5}});
        pad->pattern = QStringLiteral("SOLID");
        def->entities.push_back(std::move(pad));
    }
    doc.beginTransaction(QStringLiteral("setup"));
    auto ins = std::make_unique<InsertEntity>();
    ins->blockName = QStringLiteral("GBR-D10");
    ins->position = {30, 40};
    doc.addEntity(std::move(ins));
    doc.commitTransaction();

    // Endpoint snapping OFF: the pad center must still be reachable as a
    // CENTER snap — a drafter dimensions pad-center to pad-center.
    SnapSettings s;
    s.endpoint = false;
    const auto r = snapQuery(doc, {30.7, 40.7}, 2.0, s, std::nullopt);
    REQUIRE(r);
    REQUIRE(r->kind == SnapKind::Center);
    REQUIRE(r->point.x == Approx(30.0));
    REQUIRE(r->point.y == Approx(40.0));
}

TEST_CASE("wide gerber trace snaps at endpoints and midpoint", "[snap][gerber]")
{
    Document doc;
    doc.beginTransaction(QStringLiteral("setup"));
    auto trace = std::make_unique<PolylineEntity>(
        std::vector<PolyVertex>{{{0, 0}, 0.0}, {{10, 0}, 0.0}}, false);
    trace->setWidth(0.5); // Gerber round-aperture trace
    doc.addEntity(std::move(trace));
    doc.commitTransaction();

    SnapSettings s;
    const auto end = snapQuery(doc, {9.8, 0.3}, 1.0, s, std::nullopt);
    REQUIRE(end);
    REQUIRE(end->kind == SnapKind::Endpoint);
    REQUIRE(end->point.x == Approx(10.0));
    REQUIRE(end->point.y == Approx(0.0).margin(1e-9));

    const auto mid = snapQuery(doc, {5.1, 0.2}, 1.0, s, std::nullopt);
    REQUIRE(mid);
    REQUIRE(mid->kind == SnapKind::Midpoint);
    REQUIRE(mid->point.x == Approx(5.0));
}

// --- sketch-ref-snap: extra reference snap points fed by the front-end -------

#include <BRepAdaptor_Surface.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include "solid/SolidOps.h"

TEST_CASE("extra snap points are returned by snapQuery", "[snap][sketch-ref]")
{
    // Empty document: no entity would ever produce a snap here.
    Document doc;
    SnapSettings s;
    REQUIRE_FALSE(snapQuery(doc, {40, 30}, 5.0, s, std::nullopt));

    // Feed a reference target (a face vertex) and a center.
    doc.setExtraSnapPoints({{Vec2d{40, 30}, SnapKind::Endpoint},
                            {Vec2d{-10, 0}, SnapKind::Center}});

    const auto near = snapQuery(doc, {42, 31}, 5.0, s, std::nullopt);
    REQUIRE(near);
    REQUIRE(near->kind == SnapKind::Endpoint);
    REQUIRE(near->point.x == Approx(40.0));
    REQUIRE(near->point.y == Approx(30.0));
    REQUIRE(near->entity == kInvalidEntityId);

    const auto ctr = snapQuery(doc, {-11, 1}, 5.0, s, std::nullopt);
    REQUIRE(ctr);
    REQUIRE(ctr->kind == SnapKind::Center);
    REQUIRE(ctr->point.x == Approx(-10.0));

    // Out of tolerance: nothing.
    REQUIRE_FALSE(snapQuery(doc, {60, 60}, 5.0, s, std::nullopt));

    // Clearing drops them again.
    doc.clearExtraSnapPoints();
    REQUIRE_FALSE(snapQuery(doc, {42, 31}, 5.0, s, std::nullopt));
}

TEST_CASE("faceSnapPoints2d yields box corners and hole center", "[snap][sketch-ref]")
{
    using namespace viki;
    // 20x20x10 box; drill a hole on the top face so it carries a circular edge.
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(20.0, 20.0, 10.0).Shape();
    const WorkPlane top{gp_Pnt(0, 0, 10), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)};
    const auto bored =
        solidops::makeHole(box, top, Vec2d{10, 10}, 6.0, 4.0, false);
    REQUIRE(bored.ok);

    // Grab the top face (z==10, outward normal +Z).
    TopoDS_Shape topFace;
    for (TopExp_Explorer e(bored.shape, TopAbs_FACE); e.More(); e.Next()) {
        BRepAdaptor_Surface surf(TopoDS::Face(e.Current()));
        // pick the planar face whose normal is ~+Z at z=10
        const gp_Pnt c = surf.Value((surf.FirstUParameter() + surf.LastUParameter()) / 2,
                                    (surf.FirstVParameter() + surf.LastVParameter()) / 2);
        if (surf.GetType() == GeomAbs_Plane && std::fabs(c.Z() - 10.0) < 1e-6) {
            topFace = e.Current();
            break;
        }
    }
    REQUIRE_FALSE(topFace.IsNull());

    const auto pts = solidops::faceSnapPoints2d(topFace, top);
    REQUIRE_FALSE(pts.empty());

    // There must be a Center target at the hole center (10,10) in-plane, and an
    // Endpoint at a face corner (0,0).
    bool hasHoleCenter = false, hasCorner = false;
    for (const SnapPoint& sp : pts) {
        if (sp.kind == SnapKind::Center &&
            std::hypot(sp.p.x - 10.0, sp.p.y - 10.0) < 1e-6)
            hasHoleCenter = true;
        if (sp.kind == SnapKind::Endpoint &&
            std::hypot(sp.p.x - 0.0, sp.p.y - 0.0) < 1e-6)
            hasCorner = true;
    }
    REQUIRE(hasHoleCenter);
    REQUIRE(hasCorner);

    // And they are queryable through the Document hook + SnapEngine.
    Document doc;
    doc.setExtraSnapPoints(pts);
    SnapSettings s;
    const auto r = snapQuery(doc, {10.3, 9.8}, 1.0, s, std::nullopt);
    REQUIRE(r);
    REQUIRE(r->kind == SnapKind::Center);
    REQUIRE(r->point.x == Approx(10.0));
    REQUIRE(r->point.y == Approx(10.0));
}
