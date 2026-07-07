#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "cmd/CommandProcessor.h"
#include "doc/Entities.h"
#include "geom/GeomUtil.h"
#include "script/ScriptRunner.h"

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

TEST_CASE("LINE draws segments headless", "[commands]")
{
    Rig rig;
    const auto r = rig.processor.submit(QStringLiteral("LINE 0,0 100,0 100,50"), true);
    REQUIRE(r.ok);
    REQUIRE(rig.doc.entityCount() == 2);

    const auto* line = dynamic_cast<const LineEntity*>(rig.doc.entity(rig.doc.drawOrder()[1]));
    REQUIRE(line);
    REQUIRE(line->p2().y == Approx(50.0));
}

TEST_CASE("relative and polar coordinates", "[commands]")
{
    Rig rig;
    REQUIRE(rig.processor.submit(QStringLiteral("LINE 10,10 @5,0 @10<90"), true).ok);
    REQUIRE(rig.doc.entityCount() == 2);
    const auto* second = dynamic_cast<const LineEntity*>(rig.doc.entity(rig.doc.drawOrder()[1]));
    REQUIRE(second->p1().x == Approx(15.0));
    REQUIRE(second->p2().x == Approx(15.0).margin(1e-9));
    REQUIRE(second->p2().y == Approx(20.0));
}

TEST_CASE("CIRCLE with numeric radius and point radius", "[commands]")
{
    Rig rig;
    REQUIRE(rig.processor.submit(QStringLiteral("CIRCLE 50,50 25"), true).ok);
    const auto* c = dynamic_cast<const CircleEntity*>(rig.doc.entity(1));
    REQUIRE(c);
    REQUIRE(c->radius() == Approx(25.0));

    // Radius from a point: uses distance to center (lastPoint).
    REQUIRE(rig.processor.submit(QStringLiteral("CIRCLE 0,0 3,4"), true).ok);
    const auto* c2 = dynamic_cast<const CircleEntity*>(rig.doc.entity(2));
    REQUIRE(c2->radius() == Approx(5.0));
}

TEST_CASE("ARC through three points", "[commands]")
{
    Rig rig;
    // Half unit-circle CCW: (1,0) -> (0,1) -> (-1,0).
    REQUIRE(rig.processor.submit(QStringLiteral("ARC 1,0 0,1 -1,0"), true).ok);
    const auto* a = dynamic_cast<const ArcEntity*>(rig.doc.entity(1));
    REQUIRE(a);
    REQUIRE(a->radius() == Approx(1.0));
    REQUIRE(a->sweep() == Approx(M_PI));

    // Same points, opposite travel direction: passes below through (0,-1).
    REQUIRE(rig.processor.submit(QStringLiteral("ARC 1,0 0,-1 -1,0"), true).ok);
    const auto* b = dynamic_cast<const ArcEntity*>(rig.doc.entity(2));
    REQUIRE(b->sweep() == Approx(M_PI));
    // The arc must contain (0,-1): angle 3pi/2 from its start.
    REQUIRE(angleOnArc(1.5 * M_PI, b->startAngle(), b->sweep()));
}

TEST_CASE("ERASE by ids, UNDO, REDO", "[commands]")
{
    Rig rig;
    REQUIRE(rig.processor.submit(QStringLiteral("CIRCLE 0,0 10"), true).ok);
    REQUIRE(rig.processor.submit(QStringLiteral("CIRCLE 30,0 10"), true).ok);
    REQUIRE(rig.doc.entityCount() == 2);

    REQUIRE(rig.processor.submit(QStringLiteral("ERASE 1 2"), true).ok);
    REQUIRE(rig.doc.entityCount() == 0);

    REQUIRE(rig.processor.submit(QStringLiteral("UNDO"), true).ok);
    REQUIRE(rig.doc.entityCount() == 2);
    REQUIRE(rig.processor.submit(QStringLiteral("REDO"), true).ok);
    REQUIRE(rig.doc.entityCount() == 0);
}

TEST_CASE("ERASE consumes pre-existing selection (pickfirst)", "[commands]")
{
    Rig rig;
    REQUIRE(rig.processor.submit(QStringLiteral("CIRCLE 0,0 10"), true).ok);
    rig.selection.add(1);
    REQUIRE(rig.processor.submit(QStringLiteral("ERASE"), true).ok);
    REQUIRE(rig.doc.entityCount() == 0);
    REQUIRE(rig.selection.isEmpty());
}

TEST_CASE("unknown command and bad point are errors", "[commands]")
{
    Rig rig;
    REQUIRE_FALSE(rig.processor.submit(QStringLiteral("FROBNICATE"), true).ok);
    // Mixed alphanumeric garbage is neither a point nor a keyword.
    REQUIRE_FALSE(rig.processor.submit(QStringLiteral("LINE 0,0 12banana"), true).ok);
    REQUIRE_FALSE(rig.processor.hasActiveCommand()); // failed command cleaned up
    REQUIRE_FALSE(rig.doc.inTransaction());          // no leaked transaction
}

TEST_CASE("command aliases", "[commands]")
{
    Rig rig;
    REQUIRE(rig.processor.submit(QStringLiteral("C 0,0 1"), true).ok);
    REQUIRE(rig.processor.submit(QStringLiteral("L 0,0 1,0"), true).ok);
    REQUIRE(rig.doc.entityCount() == 2);
}

TEST_CASE("script runner: multi-line command with blank-line Enter", "[script]")
{
    Rig rig;
    const QString script = QStringLiteral(
        "# rectangle 100x50 as 4 lines\n"
        "LINE 0,0 100,0 100,50 0,50 0,0\n"
        "\n"
        "CIRCLE 50,25 10\n");
    const auto r = runScript(rig.processor, script);
    REQUIRE(r.ok);
    REQUIRE(rig.doc.entityCount() == 5);
}

TEST_CASE("script runner reports failing line", "[script]")
{
    Rig rig;
    const auto r = runScript(rig.processor,
                             QStringLiteral("CIRCLE 0,0 5\nBOGUS 1,2\n"));
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.lineNumber == 2);
}
