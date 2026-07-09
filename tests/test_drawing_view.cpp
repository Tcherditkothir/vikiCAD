#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Dir.hxx>

#include "cmd/CommandProcessor.h"
#include "doc/Entities.h"
#include "render/DrawingProjection.h"
#include "solid/SolidEntity.h"

using namespace viki;
using Catch::Approx;

namespace {

TopoDS_Shape box10()
{
    BRepPrimAPI_MakeBox mk(10.0, 10.0, 10.0);
    mk.Build();
    REQUIRE(!mk.Shape().IsNull());
    return mk.Shape();
}

struct Rig {
    Document doc;
    SelectionSet selection;
    CommandContext ctx{doc, selection};
    CommandProcessor processor{ctx};
    Rig() { registerBuiltinCommands(processor); }
    bool run(const QString& s) { return processor.submit(s, true).ok; }
};

} // namespace

TEST_CASE("projectToDrawing: 10x10x10 box top view is a 10x10 square", "[hlr]")
{
    const TopoDS_Shape box = box10();
    // Top view: look straight down -Z.
    const DrawingProjection proj =
        render::projectToDrawing(box, gp_Dir(0, 0, -1));

    REQUIRE(proj.visible.size() >= 4); // at least the 4 outline edges

    // 2D bounding box of the visible projection.
    double minx = 1e18, miny = 1e18, maxx = -1e18, maxy = -1e18;
    for (const DrawingSegment& s : proj.visible)
        for (const Vec2d& p : {s.a, s.b}) {
            minx = std::min(minx, p.x);
            miny = std::min(miny, p.y);
            maxx = std::max(maxx, p.x);
            maxy = std::max(maxy, p.y);
        }
    REQUIRE((maxx - minx) == Approx(10.0).margin(1e-6));
    REQUIRE((maxy - miny) == Approx(10.0).margin(1e-6));
}

TEST_CASE("projectToDrawing: front view of the box is also 10x10", "[hlr]")
{
    const TopoDS_Shape box = box10();
    const DrawingProjection proj =
        render::projectToDrawing(box, gp_Dir(0, 1, 0)); // look +Y (front)
    REQUIRE(proj.visible.size() >= 4);
    double minx = 1e18, miny = 1e18, maxx = -1e18, maxy = -1e18;
    for (const DrawingSegment& s : proj.visible)
        for (const Vec2d& p : {s.a, s.b}) {
            minx = std::min(minx, p.x);
            miny = std::min(miny, p.y);
            maxx = std::max(maxx, p.x);
            maxy = std::max(maxy, p.y);
        }
    REQUIRE((maxx - minx) == Approx(10.0).margin(1e-6));
    REQUIRE((maxy - miny) == Approx(10.0).margin(1e-6));
}

TEST_CASE("projectToDrawing: null shape yields empty projection", "[hlr]")
{
    const DrawingProjection proj =
        render::projectToDrawing(TopoDS_Shape(), gp_Dir(0, 0, -1));
    REQUIRE(proj.visible.empty());
    REQUIRE(proj.hidden.empty());
}

TEST_CASE("MAKEVIEW command projects a solid into 2D line entities", "[hlr]")
{
    Rig rig;
    rig.doc.beginTransaction(QStringLiteral("box"));
    const EntityId sid =
        rig.doc.addEntity(std::make_unique<SolidEntity>(box10()));
    rig.doc.commitTransaction();
    REQUIRE(rig.doc.entityCount() == 1);

    // Top view of the box.
    REQUIRE(rig.run(QStringLiteral("MAKEVIEW TOP %1").arg(sid)));

    // The solid is still there; new LineEntity outline was added.
    int lines = 0;
    BBox2d bbox;
    for (const EntityId id : rig.doc.drawOrder()) {
        if (const auto* le = dynamic_cast<const LineEntity*>(rig.doc.entity(id))) {
            ++lines;
            bbox.expand(le->bounds());
        }
    }
    REQUIRE(lines >= 4);
    REQUIRE(bbox.width() == Approx(10.0).margin(1e-6));
    REQUIRE(bbox.height() == Approx(10.0).margin(1e-6));

    // Undo removes the projected lines, leaving just the solid.
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    int remaining = 0;
    for (const EntityId id : rig.doc.drawOrder())
        if (dynamic_cast<const LineEntity*>(rig.doc.entity(id)))
            ++remaining;
    REQUIRE(remaining == 0);
}
