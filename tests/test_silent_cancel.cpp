#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS_Shape.hxx>

#include "cmd/CommandProcessor.h"
#include "doc/Entities.h"
#include "solid/SolidEntity.h"

using namespace viki;
using Catch::Approx;

// Issue #12 — "cylinder(height, radius) vs the intuitive (radius, length)".
//
// The reporter's pain was upstream of VikiCAD: the manifold3d PYTHON wrapper
// takes cylinder(height, radius_low, ...), so a hand-written tube generator
// built 100 mm discs instead of r=1.4 tubes and only the numeric check caught
// it. The doc note they asked for lives in AGENT.md now. What THIS file locks
// is the half of the trap that was VikiCAD's own: a tube built natively is
// SWEEP (circle profile + path), and the natural one-line form `SWEEP 1 2`
// fed BOTH ids to the first selection stage — the command then cancelled
// itself and the CLI answered {"ok":true} with no message at all, which is
// exactly the "looks fine, did nothing" failure the reporter was chasing.
//
// A command that gives up without a word is now an ERROR in strict/headless
// mode, and it names the cure.

namespace {

double volumeOf(const TopoDS_Shape& s)
{
    GProp_GProps props;
    BRepGProp::VolumeProperties(s, props);
    return props.Mass();
}

struct Rig {
    Document doc;
    SelectionSet selection;
    CommandContext ctx{doc, selection};
    CommandProcessor processor{ctx};
    Rig() { registerBuiltinCommands(processor); }

    // Headless (strict), the way the CLI drives it — returns the full result.
    CommandProcessor::Result run(const QString& s) { return processor.submit(s, true); }

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

TEST_CASE("issue12: SWEEP fed two selection stages on one line fails LOUDLY", "[issue12]")
{
    Rig rig;
    const EntityId circle = rig.add(std::make_unique<CircleEntity>(Vec2d{0, 0}, 1.4));
    const EntityId line =
        rig.add(std::make_unique<LineEntity>(Vec2d{0, 0}, Vec2d{0, 50}));

    // The natural mistake: both ids on the command line. The greedy EntitySet
    // gulp hands them ALL to "Select profile entities", so the path stage
    // starves. Before the fix this returned ok:true with an empty document.
    const auto r = rig.run(QStringLiteral("SWEEP %1 %2").arg(circle).arg(line));
    CHECK_FALSE(r.ok);
    CHECK(rig.solidCount() == 0);
    // The error must name the command, where it died, and the cure.
    CHECK(r.error.contains(QStringLiteral("SWEEP")));
    CHECK(r.error.contains(QStringLiteral("Select path")));
    CHECK(r.error.contains(QStringLiteral(".vks")));

    // NOTHING was consumed: the two entities are still there to be swept.
    CHECK(rig.doc.entity(circle) != nullptr);
    CHECK(rig.doc.entity(line) != nullptr);
}

TEST_CASE("issue12: the documented two-line script builds the tube", "[issue12]")
{
    Rig rig;
    const EntityId circle = rig.add(std::make_unique<CircleEntity>(Vec2d{0, 0}, 1.4));
    const EntityId line =
        rig.add(std::make_unique<LineEntity>(Vec2d{0, 0}, Vec2d{0, 50}));

    // The AGENT.md §2 rule 5 shape: one stage per submission, the way
    // runScript() feeds a .vks file (submit with strict=false, line by line).
    CHECK(rig.processor.submit(QStringLiteral("SWEEP"), false).ok);
    CHECK(rig.processor.submit(QString::number(circle), false).ok);
    CHECK(rig.processor.submit(QString::number(line), false).ok);

    REQUIRE(rig.solidCount() == 1);
    const auto* s = rig.firstSolid();
    REQUIRE(s != nullptr);
    // r=1.4 along 50 mm: pi * 1.4^2 * 50 = 307.876...
    CHECK(volumeOf(s->shape()) == Approx(M_PI * 1.4 * 1.4 * 50.0).epsilon(0.01));
}

TEST_CASE("issue12: a preselect + one id keeps working (SWEEP <path>)", "[issue12]")
{
    Rig rig;
    const EntityId circle = rig.add(std::make_unique<CircleEntity>(Vec2d{0, 0}, 1.4));
    const EntityId line =
        rig.add(std::make_unique<LineEntity>(Vec2d{0, 0}, Vec2d{0, 50}));

    // The documented one-line form: profile via pickfirst, path as the id.
    rig.selection.add(circle);
    const auto r = rig.run(QStringLiteral("SWEEP %1").arg(line));
    CHECK(r.ok);
    REQUIRE(rig.solidCount() == 1);
    const auto* s = rig.firstSolid();
    REQUIRE(s != nullptr);
    CHECK(volumeOf(s->shape()) == Approx(M_PI * 1.4 * 1.4 * 50.0).epsilon(0.01));
}

TEST_CASE("issue12: EXTRUDE Cut eats the same trap and now says so", "[issue12]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 40,30")).ok);
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 1")).ok);
    REQUIRE(rig.run(QStringLiteral("CIRCLE 30,15 4")).ok);

    // "EXTRUDE 10 CUT <target>" puts the mode keyword then ONE id; the profile
    // stage is then still hungry and the implicit Finish cancels the command.
    const auto r = rig.run(QStringLiteral("EXTRUDE 10 CUT 3"));
    CHECK_FALSE(r.ok);
    CHECK(r.error.contains(QStringLiteral("EXTRUDE")));
    // The solid is untouched by the failed attempt.
    CHECK(rig.solidCount() == 1);
}

TEST_CASE("issue12: a command that explains itself is left alone", "[issue12]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 10,10")).ok);
    REQUIRE(rig.run(QStringLiteral("RECT 20,20 30,30")).ok);

    // LOFT takes two selection stages too, but it ALWAYS explains its refusal
    // ("loft produced no solid...") — the guard must not turn its own
    // diagnostic into a generic error.
    const auto r = rig.run(QStringLiteral("LOFT 1 2"));
    CHECK(r.error.isEmpty());
    CHECK(rig.solidCount() == 0);
}

TEST_CASE("issue12: interactive (non-strict) mode stays silent", "[issue12]")
{
    Rig rig;
    const EntityId circle = rig.add(std::make_unique<CircleEntity>(Vec2d{0, 0}, 1.4));
    const EntityId line =
        rig.add(std::make_unique<LineEntity>(Vec2d{0, 0}, Vec2d{0, 50}));

    // A bare Escape in the GUI must not raise an error dialog — the guard is
    // strict-mode only, because there a cancellation IS the user's answer.
    const auto r = rig.processor.submit(
        QStringLiteral("SWEEP %1 %2").arg(circle).arg(line), /*strict=*/false);
    CHECK(r.ok);
    CHECK(r.error.isEmpty());
}
