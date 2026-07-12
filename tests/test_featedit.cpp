#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <GProp_GProps.hxx>
#include <gp_Ax2.hxx>

#include "cmd/CommandProcessor.h"
#include "solid/SolidEntity.h"
#include "solid/SolidOps.h"

// FEATEDIT — the headless twin of the Properties panel. These tests drive the
// full command grammar ("params before selection, id last") end to end:
// HOLE a box, edit the bore via FEATEDIT, verify volumes/positions, undo,
// and walk every error path.

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

SolidEntity* firstSolid(Document& doc)
{
    for (const EntityId id : doc.drawOrder())
        if (auto* s = dynamic_cast<SolidEntity*>(doc.entity(id)))
            return s;
    return nullptr;
}

double volumeOf(const TopoDS_Shape& s)
{
    GProp_GProps props;
    BRepGProp::VolumeProperties(s, props);
    return props.Mass();
}

bool anyContains(const std::vector<QString>& msgs, const QString& needle)
{
    for (const QString& m : msgs)
        if (m.contains(needle))
            return true;
    return false;
}

// 20x20x10 box with a through bore: the standard fixture. Returns the solid.
SolidEntity* boxWithHole(Rig& rig, double dia = 4.0)
{
    REQUIRE(rig.run(QStringLiteral("WORKPLANE XY")));
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 20,20")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 1"))); // vol 4000
    REQUIRE(rig.run(QStringLiteral("HOLE %1 T 10,10 2").arg(dia)));
    SolidEntity* s = firstSolid(rig.doc);
    REQUIRE(s);
    REQUIRE(s->features); // HOLE seeds the parametric history
    return s;
}

} // namespace

TEST_CASE("FEATEDIT LIST prints the editable parameters", "[featedit]")
{
    Rig rig;
    SolidEntity* s = boxWithHole(rig);
    const EntityId id = s->id();

    rig.ctx.clearMessages();
    REQUIRE(rig.run(QStringLiteral("FEATEDIT LIST %1").arg(id)));
    const auto& msgs = rig.ctx.messages();
    // Through bore: diameter + centre, no depth row. One-decimal contract.
    REQUIRE(anyContains(msgs, QStringLiteral("3 editable parameter(s)")));
    REQUIRE(anyContains(msgs, QStringLiteral("hole 1: diameter = 4.0")));
    REQUIRE(anyContains(msgs, QStringLiteral("hole 1: center x = 10.0")));
    REQUIRE(anyContains(msgs, QStringLiteral("hole 1: center y = 10.0")));
    REQUIRE_FALSE(anyContains(msgs, QStringLiteral("depth")));
}

TEST_CASE("FEATEDIT diameter changes the bore volume; UNDO restores",
          "[featedit]")
{
    Rig rig;
    SolidEntity* s = boxWithHole(rig);
    const EntityId id = s->id();
    const double before = 4000.0 - M_PI * 4.0 * 10.0; // r=2
    CHECK(volumeOf(s->shape()) == Approx(before).epsilon(1e-4));

    rig.ctx.clearMessages();
    REQUIRE(rig.run(QStringLiteral("FEATEDIT diameter 6 1 %1").arg(id)));
    REQUIRE(anyContains(rig.ctx.messages(),
                        QStringLiteral("diameter = 6.0 on node 1")));

    s = firstSolid(rig.doc);
    REQUIRE(s);
    CHECK(volumeOf(s->shape()) ==
          Approx(4000.0 - M_PI * 9.0 * 10.0).epsilon(1e-4)); // r=3
    CHECK(s->features->nodeAt(1).diameter == Approx(6.0));

    // ONE undo restores both the shape and the parameter (one transaction).
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    s = firstSolid(rig.doc);
    REQUIRE(s);
    REQUIRE(s->features);
    CHECK(volumeOf(s->shape()) == Approx(before).epsilon(1e-4));
    CHECK(s->features->nodeAt(1).diameter == Approx(4.0));

    // REDO replays the edit.
    REQUIRE(rig.run(QStringLiteral("REDO")));
    s = firstSolid(rig.doc);
    REQUIRE(s);
    CHECK(s->features->nodeAt(1).diameter == Approx(6.0));
}

TEST_CASE("FEATEDIT centerx MOVES the bore (probe cylinder)", "[featedit]")
{
    Rig rig;
    SolidEntity* s = boxWithHole(rig);
    const EntityId id = s->id();

    // Thin probe cylinder through the box: no material where the bore is.
    const auto probeAt = [](double x, double y) {
        return BRepPrimAPI_MakeCylinder(
                   gp_Ax2(gp_Pnt(x, y, -1.0), gp_Dir(0, 0, 1)), 1.0, 12.0)
            .Shape();
    };
    CHECK(solidops::interferenceVolume(s->shape(), probeAt(10, 10)) ==
          Approx(0.0).margin(1e-6));

    REQUIRE(rig.run(QStringLiteral("FEATEDIT centerx 15 1 %1").arg(id)));
    s = firstSolid(rig.doc);
    REQUIRE(s);
    // Material is back at the old centre; the bore lives at (15,10) now.
    CHECK(solidops::interferenceVolume(s->shape(), probeAt(10, 10)) ==
          Approx(M_PI * 1.0 * 10.0).epsilon(1e-3));
    CHECK(solidops::interferenceVolume(s->shape(), probeAt(15, 10)) ==
          Approx(0.0).margin(1e-6));
    CHECK(s->features->nodeAt(1).holeCenter.x == Approx(15.0));
    CHECK(s->features->nodeAt(1).holeCenter.y == Approx(10.0));

    // centery too, and the tree survives an UNDO round trip.
    REQUIRE(rig.run(QStringLiteral("FEATEDIT centery 5 1 %1").arg(id)));
    s = firstSolid(rig.doc);
    REQUIRE(s);
    CHECK(solidops::interferenceVolume(s->shape(), probeAt(15, 5)) ==
          Approx(0.0).margin(1e-6));
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    s = firstSolid(rig.doc);
    REQUIRE(s);
    CHECK(solidops::interferenceVolume(s->shape(), probeAt(10, 10)) ==
          Approx(0.0).margin(1e-6));
}

TEST_CASE("FEATEDIT error paths report and leave the solid untouched",
          "[featedit]")
{
    Rig rig;
    SolidEntity* s = boxWithHole(rig);
    const EntityId holeId = s->id();
    const double vol = volumeOf(s->shape());

    SECTION("not a solid")
    {
        REQUIRE(rig.run(QStringLiteral("CIRCLE 100,100 5")));
        EntityId circleId = kInvalidEntityId;
        for (const EntityId eid : rig.doc.drawOrder())
            if (!dynamic_cast<SolidEntity*>(rig.doc.entity(eid)))
                circleId = eid;
        REQUIRE(circleId != kInvalidEntityId);
        rig.ctx.clearMessages();
        rig.run(QStringLiteral("FEATEDIT diameter 6 1 %1").arg(circleId));
        REQUIRE(anyContains(rig.ctx.messages(),
                            QStringLiteral("that id is not a solid")));
    }

    SECTION("no feature history")
    {
        // A bare extrusion records no FeatureTree (only HOLE/SHELL do).
        REQUIRE(rig.run(QStringLiteral("RECT 100,0 120,20")));
        REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 %1")
                            .arg(rig.doc.drawOrder().back())));
        EntityId bareId = kInvalidEntityId;
        for (const EntityId eid : rig.doc.drawOrder()) {
            auto* sol = dynamic_cast<SolidEntity*>(rig.doc.entity(eid));
            if (sol && !sol->features)
                bareId = eid;
        }
        REQUIRE(bareId != kInvalidEntityId);
        rig.ctx.clearMessages();
        rig.run(QStringLiteral("FEATEDIT diameter 6 1 %1").arg(bareId));
        REQUIRE(anyContains(rig.ctx.messages(),
                            QStringLiteral("no feature history")));
        rig.ctx.clearMessages();
        rig.run(QStringLiteral("FEATEDIT LIST %1").arg(bareId));
        REQUIRE(anyContains(rig.ctx.messages(),
                            QStringLiteral("no feature history")));
    }

    SECTION("param/node mismatch rolls back")
    {
        rig.ctx.clearMessages();
        // Node 0 is the BaseShape: no diameter there.
        rig.run(QStringLiteral("FEATEDIT diameter 6 0 %1").arg(holeId));
        REQUIRE(anyContains(rig.ctx.messages(),
                            QStringLiteral("no editable 'diameter' on node 0")));
        rig.ctx.clearMessages();
        // thickness on a hole node: kind mismatch.
        rig.run(QStringLiteral("FEATEDIT thickness 2 1 %1").arg(holeId));
        REQUIRE(anyContains(rig.ctx.messages(),
                            QStringLiteral("no editable 'thickness' on node 1")));
        rig.ctx.clearMessages();
        // Negative length is rejected by featureparams::set.
        rig.run(QStringLiteral("FEATEDIT diameter -3 1 %1").arg(holeId));
        REQUIRE(anyContains(rig.ctx.messages(),
                            QStringLiteral("no editable 'diameter' on node 1")));

        s = firstSolid(rig.doc);
        REQUIRE(s);
        CHECK(volumeOf(s->shape()) == Approx(vol).epsilon(1e-6));
        CHECK(s->features->nodeAt(1).diameter == Approx(4.0));
        // The failed edits left NOTHING on the undo stack: undo pops the HOLE.
        REQUIRE(rig.run(QStringLiteral("UNDO")));
        s = firstSolid(rig.doc);
        REQUIRE(s);
        CHECK(volumeOf(s->shape()) == Approx(4000.0).epsilon(1e-6));
    }

    SECTION("unknown parameter and bad node index")
    {
        rig.ctx.clearMessages();
        rig.run(QStringLiteral("FEATEDIT radius 6 1 %1").arg(holeId));
        REQUIRE(anyContains(rig.ctx.messages(),
                            QStringLiteral("unknown parameter 'radius'")));
        rig.ctx.clearMessages();
        rig.run(QStringLiteral("FEATEDIT diameter 6 1.5 %1").arg(holeId));
        REQUIRE(anyContains(
            rig.ctx.messages(),
            QStringLiteral("node index must be a non-negative integer")));
        s = firstSolid(rig.doc);
        REQUIRE(s);
        CHECK(volumeOf(s->shape()) == Approx(vol).epsilon(1e-6));
    }
}

TEST_CASE("FEATEDIT edits a shell wall and an extrude height", "[featedit]")
{
    // SHELL records its thickness; FEATEDIT drives it like the panel does.
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("WORKPLANE XY")));
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 10,10")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 1")));
    REQUIRE(rig.run(QStringLiteral("SHELL 1 2")));
    SolidEntity* s = firstSolid(rig.doc);
    REQUIRE(s);
    REQUIRE(s->features);
    const EntityId id = s->id();
    CHECK(volumeOf(s->shape()) == Approx(1000.0 - 512.0).epsilon(1e-3));

    REQUIRE(rig.run(QStringLiteral("FEATEDIT thickness 2 1 %1").arg(id)));
    s = firstSolid(rig.doc);
    REQUIRE(s);
    CHECK(volumeOf(s->shape()) ==
          Approx(1000.0 - 6.0 * 6.0 * 6.0).epsilon(1e-3));
    CHECK(s->features->nodeAt(1).thickness == Approx(2.0));
}
