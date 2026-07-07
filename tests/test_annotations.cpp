#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "cmd/CommandProcessor.h"
#include "doc/Annotations.h"
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
    bool run(const QString& s) { return processor.submit(s, true).ok; }
    PrimitiveList render(EntityId id)
    {
        RenderContext rc;
        rc.doc = &doc;
        PrimitiveList out;
        doc.entity(id)->buildPrimitives(rc, out);
        return out;
    }
};
} // namespace

TEST_CASE("TEXT creates multiline text", "[m4]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("TEXT 10,20 5 0 Hello world\\nsecond line")));
    const auto* t = dynamic_cast<const TextEntity*>(rig.doc.entity(1));
    REQUIRE(t);
    REQUIRE(t->height() == Approx(5.0));
    REQUIRE(t->text().contains(QLatin1Char('\n')));
    const auto prims = rig.render(1);
    REQUIRE(prims.texts.size() == 2);
    REQUIRE(prims.strokes.empty()); // pick box only under forHitTest
}

TEST_CASE("DIMLINEAR measures along the chosen axis and follows units", "[m4]")
{
    Rig rig;
    // Points offset in y; dim placed below → horizontal measurement.
    REQUIRE(rig.run(QStringLiteral("DIMLINEAR 0,0 50.8,7 20,-20")));
    const auto* d = dynamic_cast<const DimensionEntity*>(rig.doc.entity(1));
    REQUIRE(d);
    REQUIRE(d->kind == DimensionEntity::Kind::Linear);
    REQUIRE(d->measurement() == Approx(50.8));

    auto prims = rig.render(1);
    REQUIRE(prims.texts.size() == 1);
    REQUIRE(prims.texts[0].text == QStringLiteral("50.80"));
    // Arrows are two filled strokes.
    int filled = 0;
    for (const auto& s : prims.strokes)
        filled += s.filled ? 1 : 0;
    REQUIRE(filled == 2);

    // Flip display units: the SAME entity reformats.
    rig.doc.setDisplayUnits(DisplayUnits::Inches);
    prims = rig.render(1);
    REQUIRE(prims.texts[0].text == QStringLiteral("2.00"));
}

TEST_CASE("DIMALIGNED measures point distance", "[m4]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("DIMALIGNED 0,0 30,40 20,30")));
    const auto* d = dynamic_cast<const DimensionEntity*>(rig.doc.entity(1));
    REQUIRE(d->measurement() == Approx(50.0));
}

TEST_CASE("DIMANGULAR measures the sweep containing the arc position", "[m4]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("DIMANGULAR 0,0 10,0 0,10 7,7")));
    const auto* d = dynamic_cast<const DimensionEntity*>(rig.doc.entity(1));
    REQUIRE(d->measurement() == Approx(M_PI_2));
    // Same legs but position on the reflex side.
    REQUIRE(rig.run(QStringLiteral("DIMANGULAR 0,0 10,0 0,10 -7,-7")));
    const auto* d2 = dynamic_cast<const DimensionEntity*>(rig.doc.entity(2));
    REQUIRE(d2->measurement() == Approx(1.5 * M_PI));
}

TEST_CASE("DIMRADIUS and DIMDIAMETER pick the curve", "[m4]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("CIRCLE 100,100 25")));
    REQUIRE(rig.run(QStringLiteral("DIMRADIUS 125,100 140,110")));
    const auto* r = dynamic_cast<const DimensionEntity*>(rig.doc.entity(2));
    REQUIRE(r);
    REQUIRE(r->measurement() == Approx(25.0));
    auto prims = rig.render(2);
    REQUIRE(prims.texts[0].text == QStringLiteral("R25.00"));

    REQUIRE(rig.run(QStringLiteral("DIMDIAMETER 75,100 50,90")));
    const auto* dd = dynamic_cast<const DimensionEntity*>(rig.doc.entity(3));
    REQUIRE(dd->measurement() == Approx(50.0));
}

TEST_CASE("DIMSTYLE changes reflow existing dimensions", "[m4]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("DIMLINEAR 0,0 10,0 5,-10")));
    REQUIRE(rig.run(QStringLiteral("DIMSTYLE DEC 1")));
    auto prims = rig.render(1);
    REQUIRE(prims.texts[0].text == QStringLiteral("10.0"));
    REQUIRE(rig.run(QStringLiteral("DIMSTYLE TH 7")));
    prims = rig.render(1);
    REQUIRE(prims.texts[0].height == Approx(7.0));
}

TEST_CASE("LEADER with text", "[m4]")
{
    Rig rig;
    // Script flow: points, blank line (Enter), then the text line.
    const auto r = runScript(rig.processor,
                             QStringLiteral("LEADER 0,0 15,15 30,15\n\nCheck this weld\n"));
    REQUIRE(r.ok);
    const auto* l = dynamic_cast<const LeaderEntity*>(rig.doc.entity(1));
    REQUIRE(l);
    REQUIRE(l->points.size() == 3);
    REQUIRE(l->text == QStringLiteral("Check this weld"));
    const auto prims = rig.render(1);
    REQUIRE(prims.texts.size() == 1);
}

TEST_CASE("HATCH from circle boundary generates pattern lines", "[m4]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("CIRCLE 0,0 10")));
    REQUIRE(rig.run(QStringLiteral("HATCH ANSI31 1 1")));
    const auto* h = dynamic_cast<const HatchEntity*>(rig.doc.entity(2));
    REQUIRE(h);
    REQUIRE(h->rings.size() == 1);
    const auto prims = rig.render(2);
    // Boundary ring + a healthy number of 45° pattern segments.
    REQUIRE(prims.strokes.size() > 4);

    // SOLID over a rectangle → one filled primitive (+ nothing else).
    REQUIRE(rig.run(QStringLiteral("RECT 50,0 80,20")));
    REQUIRE(rig.run(QStringLiteral("HATCH SOLID 1 3")));
    const auto* hs = dynamic_cast<const HatchEntity*>(rig.doc.entity(4));
    const auto sp = rig.render(hs->id());
    REQUIRE(sp.strokes.size() == 1);
    REQUIRE(sp.strokes[0].filled);
}
