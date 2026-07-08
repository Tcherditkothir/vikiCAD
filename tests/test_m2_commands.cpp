#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "cmd/CommandProcessor.h"
#include "doc/Entities.h"
#include "doc/EntitiesEx.h"

using namespace viki;
using Catch::Approx;

namespace {
struct Rig {
    Document doc;
    SelectionSet selection;
    CommandContext ctx{doc, selection};
    CommandProcessor processor{ctx};
    Rig() { registerBuiltinCommands(processor); }
};
} // namespace

TEST_CASE("RECT creates a closed polyline", "[m2]")
{
    Rig rig;
    REQUIRE(rig.processor.submit(QStringLiteral("RECT 0,0 100,50"), true).ok);
    const auto* pl = dynamic_cast<const PolylineEntity*>(rig.doc.entity(1));
    REQUIRE(pl);
    REQUIRE(pl->isClosed());
    REQUIRE(pl->vertices().size() == 4);
    REQUIRE(pl->bounds().max.x == Approx(100.0));
}

TEST_CASE("PLINE with close keyword", "[m2]")
{
    Rig rig;
    REQUIRE(rig.processor.submit(QStringLiteral("PLINE 0,0 10,0 10,10 C"), true).ok);
    const auto* pl = dynamic_cast<const PolylineEntity*>(rig.doc.entity(1));
    REQUIRE(pl);
    REQUIRE(pl->isClosed());
    REQUIRE(pl->vertices().size() == 3);
}

TEST_CASE("inch units input converts to mm storage", "[m2][units]")
{
    Rig rig;
    rig.doc.setDisplayUnits(DisplayUnits::Inches);
    REQUIRE(rig.processor.submit(QStringLiteral("LINE 0,0 1,0"), true).ok);
    const auto* line = dynamic_cast<const LineEntity*>(rig.doc.entity(1));
    REQUIRE(line->p2().x == Approx(25.4)); // 1 inch stored as mm

    // Explicit mm suffix overrides inch mode.
    REQUIRE(rig.processor.submit(QStringLiteral("LINE 0,0 10mm,0"), true).ok);
    const auto* line2 = dynamic_cast<const LineEntity*>(rig.doc.entity(2));
    REQUIRE(line2->p2().x == Approx(10.0));

    // And the quote suffix forces inches even in mm mode.
    rig.doc.setDisplayUnits(DisplayUnits::Millimeters);
    REQUIRE(rig.processor.submit(QStringLiteral("CIRCLE 0,0 2\""), true).ok);
    const auto* c = dynamic_cast<const CircleEntity*>(rig.doc.entity(3));
    REQUIRE(c->radius() == Approx(50.8));
}

TEST_CASE("MOVE and COPY", "[m2]")
{
    Rig rig;
    REQUIRE(rig.processor.submit(QStringLiteral("CIRCLE 0,0 5"), true).ok);
    REQUIRE(rig.processor.submit(QStringLiteral("MOVE 1 0,0 100,50"), true).ok);
    const auto* c = dynamic_cast<const CircleEntity*>(rig.doc.entity(1));
    REQUIRE(c->center().x == Approx(100.0));
    REQUIRE(c->center().y == Approx(50.0));

    REQUIRE(rig.processor.submit(QStringLiteral("COPY 1 0,0 50,0"), true).ok);
    REQUIRE(rig.doc.entityCount() == 2);
    const auto* copy = dynamic_cast<const CircleEntity*>(rig.doc.entity(2));
    REQUIRE(copy->center().x == Approx(150.0));

    // Undo of COPY removes the duplicate only.
    REQUIRE(rig.processor.submit(QStringLiteral("UNDO"), true).ok);
    REQUIRE(rig.doc.entityCount() == 1);
}

TEST_CASE("ROTATE by angle and by point", "[m2]")
{
    Rig rig;
    REQUIRE(rig.processor.submit(QStringLiteral("LINE 10,0 20,0"), true).ok);
    REQUIRE(rig.processor.submit(QStringLiteral("ROTATE 1 0,0 90"), true).ok);
    const auto* l = dynamic_cast<const LineEntity*>(rig.doc.entity(1));
    REQUIRE(l->p1().x == Approx(0.0).margin(1e-9));
    REQUIRE(l->p1().y == Approx(10.0));
}

TEST_CASE("MIRROR keeps source by default, erases on Y", "[m2]")
{
    Rig rig;
    REQUIRE(rig.processor.submit(QStringLiteral("CIRCLE 10,0 2"), true).ok);
    // Mirror about the Y axis (x=0 line), keep original.
    REQUIRE(rig.processor.submit(QStringLiteral("MIRROR 1 0,0 0,10 N"), true).ok);
    REQUIRE(rig.doc.entityCount() == 2);
    const auto* m = dynamic_cast<const CircleEntity*>(rig.doc.entity(2));
    REQUIRE(m->center().x == Approx(-10.0));

    REQUIRE(rig.processor.submit(QStringLiteral("MIRROR 2 0,0 10,0 Y"), true).ok);
    REQUIRE(rig.doc.entityCount() == 2); // erased source, added mirror
}

TEST_CASE("SCALE", "[m2]")
{
    Rig rig;
    REQUIRE(rig.processor.submit(QStringLiteral("CIRCLE 10,10 5"), true).ok);
    REQUIRE(rig.processor.submit(QStringLiteral("SCALE 1 0,0 2"), true).ok);
    const auto* c = dynamic_cast<const CircleEntity*>(rig.doc.entity(1));
    REQUIRE(c->center().x == Approx(20.0));
    REQUIRE(c->radius() == Approx(10.0));
}

TEST_CASE("entities land on the current layer", "[m2][layers]")
{
    Rig rig;
    const LayerId walls = rig.doc.ensureLayer(QStringLiteral("walls"), 0xFF0000);
    rig.doc.setCurrentLayer(walls);
    REQUIRE(rig.processor.submit(QStringLiteral("CIRCLE 0,0 1"), true).ok);
    REQUIRE(rig.doc.entity(1)->layerId() == walls);
    REQUIRE(rig.doc.resolveColor(*rig.doc.entity(1)) == 0xFF0000);
}

TEST_CASE("CIRCLE construction modes: 2P, 3P, diameter", "[m2][modes]")
{
    Rig rig;
    REQUIRE(rig.processor.submit(QStringLiteral("CIRCLE 2P 0,0 20,0"), true).ok);
    const auto* c1 = dynamic_cast<const CircleEntity*>(rig.doc.entity(1));
    REQUIRE(c1->center().x == Approx(10.0));
    REQUIRE(c1->radius() == Approx(10.0));

    REQUIRE(rig.processor.submit(QStringLiteral("CIRCLE 3P 1,0 0,1 -1,0"), true).ok);
    const auto* c2 = dynamic_cast<const CircleEntity*>(rig.doc.entity(2));
    REQUIRE(c2->radius() == Approx(1.0));

    REQUIRE(rig.processor.submit(QStringLiteral("CIRCLE 50,50 D 30"), true).ok);
    const auto* c3 = dynamic_cast<const CircleEntity*>(rig.doc.entity(3));
    REQUIRE(c3->radius() == Approx(15.0));
}

TEST_CASE("ARC center mode (CE)", "[m2][modes]")
{
    Rig rig;
    // Center 0,0, start at (10,0), end toward (0,10): quarter arc CCW.
    REQUIRE(rig.processor.submit(QStringLiteral("ARC CE 0,0 10,0 0,10"), true).ok);
    const auto* a = dynamic_cast<const ArcEntity*>(rig.doc.entity(1));
    REQUIRE(a);
    REQUIRE(a->radius() == Approx(10.0));
    REQUIRE(a->sweep() == Approx(M_PI_2).margin(1e-9));
}
