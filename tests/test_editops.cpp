#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "cmd/CommandProcessor.h"
#include "doc/Entities.h"
#include "doc/EntitiesEx.h"
#include "geom/GeomUtil.h"

using namespace viki;
using Catch::Approx;

namespace {
struct Rig {
    Document doc;
    SelectionSet selection;
    CommandContext ctx{doc, selection};
    CommandProcessor processor{ctx};
    Rig() { registerBuiltinCommands(processor); }
    bool run(const QString& s) { return processor.submit(s, true).ok; }
};
} // namespace

TEST_CASE("TRIM line between two cutters", "[m3]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("LINE 0,0 100,0")));   // 1: target
    REQUIRE(rig.run(QStringLiteral("LINE 30,-10 30,10"))); // 2: cutter
    REQUIRE(rig.run(QStringLiteral("LINE 70,-10 70,10"))); // 3: cutter
    // Trim the middle: cutters 2 and 3, pick at (50,0).
    REQUIRE(rig.run(QStringLiteral("TRIM 2 3 50,0")));
    // Target replaced by two segments [0,30] and [70,100].
    REQUIRE(rig.doc.entityCount() == 4);
    std::vector<double> xs;
    for (const EntityId id : rig.doc.drawOrder())
        if (const auto* l = dynamic_cast<const LineEntity*>(rig.doc.entity(id)))
            if (nearZero(l->p1().y) && nearZero(l->p2().y)) {
                xs.push_back(l->p1().x);
                xs.push_back(l->p2().x);
            }
    std::sort(xs.begin(), xs.end());
    REQUIRE(xs == std::vector<double>{0.0, 30.0, 70.0, 100.0});
}

TEST_CASE("TRIM circle to arc", "[m3]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("CIRCLE 0,0 10")));      // 1
    REQUIRE(rig.run(QStringLiteral("LINE -20,0 20,0")));    // 2 (cuts at ±10,0)
    // Remove the upper half: pick at (0,10).
    REQUIRE(rig.run(QStringLiteral("TRIM 2 0,10")));
    const auto* arc = dynamic_cast<const ArcEntity*>(rig.doc.entity(3));
    REQUIRE(arc);
    REQUIRE(arc->sweep() == Approx(M_PI).margin(1e-9));
    // Remaining arc passes through (0,-10).
    REQUIRE(angleOnArc(1.5 * M_PI, arc->startAngle(), arc->sweep()));
}

TEST_CASE("EXTEND line to boundary", "[m3]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("LINE 0,0 50,0")));     // 1: to extend
    REQUIRE(rig.run(QStringLiteral("LINE 80,-10 80,10"))); // 2: boundary
    REQUIRE(rig.run(QStringLiteral("EXTEND 2 45,0")));     // pick near the right end
    const auto* l = dynamic_cast<const LineEntity*>(rig.doc.entity(1));
    REQUIRE(l->p2().x == Approx(80.0));
}

TEST_CASE("OFFSET line, circle and closed polyline", "[m3]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("LINE 0,0 100,0")));
    REQUIRE(rig.run(QStringLiteral("OFFSET 5 50,0 50,20"))); // above
    const auto* l = dynamic_cast<const LineEntity*>(rig.doc.entity(2));
    REQUIRE(l);
    REQUIRE(l->p1().y == Approx(5.0));

    REQUIRE(rig.run(QStringLiteral("CIRCLE 200,0 10")));
    REQUIRE(rig.run(QStringLiteral("OFFSET 3 210,0 220,0"))); // outside
    const auto* c = dynamic_cast<const CircleEntity*>(rig.doc.entity(4));
    REQUIRE(c->radius() == Approx(13.0));

    REQUIRE(rig.run(QStringLiteral("RECT 300,0 400,50")));
    REQUIRE(rig.run(QStringLiteral("OFFSET 5 300,25 290,25"))); // outward (left side)
    const auto* pl = dynamic_cast<const PolylineEntity*>(rig.doc.entity(6));
    REQUIRE(pl);
    REQUIRE(pl->isClosed());
    const BBox2d b = pl->bounds();
    REQUIRE(b.width() == Approx(110.0));
    REQUIRE(b.height() == Approx(60.0));
}

TEST_CASE("FILLET with radius trims lines and adds arc", "[m3]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("LINE 0,0 100,0")));  // 1
    REQUIRE(rig.run(QStringLiteral("LINE 0,0 0,100")));  // 2
    REQUIRE(rig.run(QStringLiteral("FILLET R 10 50,0 0,50")));
    REQUIRE(rig.doc.entityCount() == 3);
    const auto* arc = dynamic_cast<const ArcEntity*>(rig.doc.entity(3));
    REQUIRE(arc);
    REQUIRE(arc->radius() == Approx(10.0));
    REQUIRE(arc->center().x == Approx(10.0));
    REQUIRE(arc->center().y == Approx(10.0));
    // Lines now end at the tangent points.
    const auto* l1 = dynamic_cast<const LineEntity*>(rig.doc.entity(1));
    REQUIRE(l1->p2().x == Approx(10.0));
    REQUIRE(l1->p2().y == Approx(0.0).margin(1e-9));
}

TEST_CASE("CHAMFER", "[m3]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("LINE 0,0 100,0")));
    REQUIRE(rig.run(QStringLiteral("LINE 0,0 0,100")));
    REQUIRE(rig.run(QStringLiteral("CHAMFER D 10 15 50,0 0,50")));
    REQUIRE(rig.doc.entityCount() == 3);
    const auto* ch = dynamic_cast<const LineEntity*>(rig.doc.entity(3));
    REQUIRE(ch);
    const BBox2d b = ch->bounds();
    REQUIRE(b.max.x == Approx(10.0));
    REQUIRE(b.max.y == Approx(15.0));
}

TEST_CASE("BREAK removes a gap; single point splits", "[m3]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("LINE 0,0 100,0")));
    REQUIRE(rig.run(QStringLiteral("BREAK 30,0 70,0")));
    REQUIRE(rig.doc.entityCount() == 2);

    REQUIRE(rig.run(QStringLiteral("CIRCLE 0,50 10")));
    // Break circle between angles 0 and 90 -> arc.
    REQUIRE(rig.run(QStringLiteral("BREAK 10,50 0,60")));
    int arcs = 0;
    for (const EntityId id : rig.doc.drawOrder())
        if (dynamic_cast<const ArcEntity*>(rig.doc.entity(id)))
            ++arcs;
    REQUIRE(arcs == 1);
}

TEST_CASE("JOIN collinear lines and chains", "[m3]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("LINE 0,0 50,0")));
    REQUIRE(rig.run(QStringLiteral("LINE 50,0 100,0")));
    REQUIRE(rig.run(QStringLiteral("JOIN 1 2")));
    REQUIRE(rig.doc.entityCount() == 1);
    const auto* l = dynamic_cast<const LineEntity*>(rig.doc.entity(3));
    REQUIRE(l);
    REQUIRE(l->p2().x == Approx(100.0));

    // L-chain becomes a polyline.
    REQUIRE(rig.run(QStringLiteral("LINE 0,10 50,10")));
    REQUIRE(rig.run(QStringLiteral("LINE 50,10 50,60")));
    REQUIRE(rig.run(QStringLiteral("JOIN 4 5")));
    bool foundPl = false;
    for (const EntityId id : rig.doc.drawOrder())
        if (const auto* pl = dynamic_cast<const PolylineEntity*>(rig.doc.entity(id))) {
            foundPl = true;
            REQUIRE(pl->vertices().size() == 3);
        }
    REQUIRE(foundPl);
}

TEST_CASE("EXPLODE polyline into lines and arcs", "[m3]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 10,10")));
    REQUIRE(rig.run(QStringLiteral("EXPLODE 1")));
    REQUIRE(rig.doc.entityCount() == 4);
    for (const EntityId id : rig.doc.drawOrder())
        REQUIRE(dynamic_cast<const LineEntity*>(rig.doc.entity(id)));
}

TEST_CASE("STRETCH moves only vertices in the window", "[m3]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("LINE 0,0 100,0")));
    // Crossing window around the right end, then move +10,+20.
    REQUIRE(rig.run(QStringLiteral("STRETCH 90,-10 110,10 100,0 110,20")));
    const auto* l = dynamic_cast<const LineEntity*>(rig.doc.entity(1));
    REQUIRE(l->p1().x == Approx(0.0));  // untouched
    REQUIRE(l->p2().x == Approx(110.0));
    REQUIRE(l->p2().y == Approx(20.0));
}

TEST_CASE("MATCHPROP copies layer and color", "[m3]")
{
    Rig rig;
    const LayerId walls = rig.doc.ensureLayer(QStringLiteral("walls"), 0xFF0000);
    rig.doc.setCurrentLayer(walls);
    REQUIRE(rig.run(QStringLiteral("CIRCLE 0,0 5")));    // 1 on walls
    rig.doc.setCurrentLayer(0);
    REQUIRE(rig.run(QStringLiteral("CIRCLE 50,0 5")));   // 2 on 0
    REQUIRE(rig.run(QStringLiteral("MATCHPROP 5,0 55,0")));
    REQUIRE(rig.doc.entity(2)->layerId() == walls);
}
