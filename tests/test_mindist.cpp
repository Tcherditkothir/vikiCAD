// MINDIST — the CAM clearance measurement. Every expected value below is
// computed BY HAND in the comment next to it; the kernel must reproduce it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <QJsonDocument>
#include <QJsonObject>

#include "cmd/CommandProcessor.h"
#include "doc/Annotations.h"
#include "doc/Block.h"
#include "doc/Entities.h"
#include "doc/EntitiesEx.h"
#include "edit/MinDist.h"

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

EntityId addTrace(Document& doc, const Vec2d& a, const Vec2d& b, double width)
{
    auto pl = std::make_unique<PolylineEntity>(
        std::vector<PolyVertex>{{a, 0.0}, {b, 0.0}}, false);
    pl->setWidth(width);
    return doc.addEntity(std::move(pl));
}

// A flashed rectangular pad: GBR-style block (one SOLID hatch ring centered
// on the flash origin) + an insert at `at`.
EntityId addRectPad(Document& doc, const QString& block, const Vec2d& at,
                    double w, double h)
{
    if (!doc.blockByName(block)) {
        BlockDef* def = doc.createBlock(block, {0, 0});
        auto hatch = std::make_unique<HatchEntity>();
        hatch->rings.push_back({{-w / 2, -h / 2},
                                {w / 2, -h / 2},
                                {w / 2, h / 2},
                                {-w / 2, h / 2}});
        hatch->pattern = QStringLiteral("SOLID");
        def->entities.push_back(std::move(hatch));
    }
    auto ins = std::make_unique<InsertEntity>();
    ins->blockName = block;
    ins->position = at;
    return doc.addEntity(std::move(ins));
}

} // namespace

TEST_CASE("MINDIST: parallel traces, width subtracted on both sides", "[mindist]")
{
    Document doc;
    doc.beginTransaction(QStringLiteral("setup"));
    // Trace A: y=0, width 0.5 -> material edge at y = 0.25.
    // Trace B: y=2, width 0.3 -> material edge at y = 1.85.
    // Edge-to-edge = 2.0 - 0.25 - 0.15 = 1.6 mm.
    const EntityId a = addTrace(doc, {0, 0}, {10, 0}, 0.5);
    const EntityId b = addTrace(doc, {0, 2}, {10, 2}, 0.3);
    doc.commitTransaction();

    const auto r = measure::minDistance(doc, a, b);
    REQUIRE(r.ok);
    REQUIRE_FALSE(r.overlap);
    REQUIRE(r.exact);
    REQUIRE(r.distance == Approx(1.6).epsilon(1e-9));
    REQUIRE(r.pa.y == Approx(0.25).epsilon(1e-9));
    REQUIRE(r.pb.y == Approx(1.85).epsilon(1e-9));
    REQUIRE(r.pa.x == Approx(r.pb.x).margin(1e-9)); // shortest path is vertical
}

TEST_CASE("MINDIST: trace vs drill circle", "[mindist]")
{
    Document doc;
    doc.beginTransaction(QStringLiteral("setup"));
    // Trace y=0 width 0.4 (edge at y=0.2); drill r=1 at (5,5) (edge at y=4).
    // Edge-to-edge = 5 - 1 - 0.2 = 3.8 mm, along x=5.
    const EntityId t = addTrace(doc, {0, 0}, {10, 0}, 0.4);
    const EntityId c = doc.addEntity(std::make_unique<CircleEntity>(Vec2d{5, 5}, 1.0));
    doc.commitTransaction();

    const auto r = measure::minDistance(doc, t, c);
    REQUIRE(r.ok);
    REQUIRE(r.exact);
    REQUIRE(r.distance == Approx(3.8).epsilon(1e-9));
    REQUIRE(r.pa.x == Approx(5.0).margin(1e-9));
    REQUIRE(r.pa.y == Approx(0.2).epsilon(1e-9));
    REQUIRE(r.pb.x == Approx(5.0).margin(1e-9));
    REQUIRE(r.pb.y == Approx(4.0).epsilon(1e-9));
}

TEST_CASE("MINDIST: two drills, center distance minus radii", "[mindist]")
{
    Document doc;
    doc.beginTransaction(QStringLiteral("setup"));
    // Centers 6 mm apart, radii 0.5 and 1.5 -> 6 - 0.5 - 1.5 = 4 mm.
    const EntityId a = doc.addEntity(std::make_unique<CircleEntity>(Vec2d{0, 0}, 0.5));
    const EntityId b = doc.addEntity(std::make_unique<CircleEntity>(Vec2d{6, 0}, 1.5));
    doc.commitTransaction();

    const auto r = measure::minDistance(doc, a, b);
    REQUIRE(r.ok);
    REQUIRE(r.distance == Approx(4.0).epsilon(1e-9));
    REQUIRE(r.pa.x == Approx(0.5).epsilon(1e-9));
    REQUIRE(r.pb.x == Approx(4.5).epsilon(1e-9));
}

TEST_CASE("MINDIST: rectangular pads edge to edge (real block footprint)",
          "[mindist]")
{
    Document doc;
    doc.beginTransaction(QStringLiteral("setup"));
    // Two 2x1 mm pads flashed at x=0 and x=5: gap = 5 - 1 - 1 = 3 mm.
    const EntityId a = addRectPad(doc, QStringLiteral("GBR-D10"), {0, 0}, 2, 1);
    const EntityId b = addRectPad(doc, QStringLiteral("GBR-D10"), {5, 0}, 2, 1);
    doc.commitTransaction();

    const auto r = measure::minDistance(doc, a, b);
    REQUIRE(r.ok);
    REQUIRE(r.exact); // real footprint, NOT a bbox fallback
    REQUIRE(r.notes.isEmpty());
    REQUIRE(r.distance == Approx(3.0).epsilon(1e-9));
    REQUIRE(r.pa.x == Approx(1.0).epsilon(1e-9));
    REQUIRE(r.pb.x == Approx(4.0).epsilon(1e-9));
}

TEST_CASE("MINDIST: pad vs trace honors the insert transform", "[mindist]")
{
    Document doc;
    doc.beginTransaction(QStringLiteral("setup"));
    // 2x1 pad at (0,10): bottom edge y = 9.5. Trace y=0 width 1 -> edge 0.5.
    // Gap = 9.5 - 0.5 = 9 mm.
    const EntityId p = addRectPad(doc, QStringLiteral("GBR-D11"), {0, 10}, 2, 1);
    const EntityId t = addTrace(doc, {-5, 0}, {5, 0}, 1.0);
    doc.commitTransaction();

    const auto r = measure::minDistance(doc, p, t);
    REQUIRE(r.ok);
    REQUIRE(r.exact);
    REQUIRE(r.distance == Approx(9.0).epsilon(1e-9));
    REQUIRE(r.pa.y == Approx(9.5).epsilon(1e-9));
    REQUIRE(r.pb.y == Approx(0.5).epsilon(1e-9));
}

TEST_CASE("MINDIST: crossing traces overlap at distance 0", "[mindist]")
{
    Document doc;
    doc.beginTransaction(QStringLiteral("setup"));
    const EntityId a = addTrace(doc, {0, 0}, {10, 0}, 0.2);
    const EntityId b = addTrace(doc, {5, -5}, {5, 5}, 0.2);
    doc.commitTransaction();

    const auto r = measure::minDistance(doc, a, b);
    REQUIRE(r.ok);
    REQUIRE(r.overlap);
    REQUIRE(r.distance == 0.0);
}

TEST_CASE("MINDIST: drill fully inside its pad = overlap (containment)",
          "[mindist]")
{
    Document doc;
    doc.beginTransaction(QStringLiteral("setup"));
    // 0.3 mm drill centered in a 2x1 pad: the boundary gap is positive but
    // the material overlaps — containment must be detected.
    const EntityId p = addRectPad(doc, QStringLiteral("GBR-D12"), {0, 0}, 2, 1);
    const EntityId d = doc.addEntity(std::make_unique<CircleEntity>(Vec2d{0, 0}, 0.3));
    doc.commitTransaction();

    const auto r = measure::minDistance(doc, p, d);
    REQUIRE(r.ok);
    REQUIRE(r.overlap);
    REQUIRE(r.distance == 0.0);
}

TEST_CASE("MINDIST: text falls back to bbox and says so", "[mindist]")
{
    Document doc;
    doc.beginTransaction(QStringLiteral("setup"));
    const EntityId t = doc.addEntity(std::make_unique<TextEntity>(
        Vec2d{0, 0}, 3.5, 0.0, QStringLiteral("REF")));
    const EntityId c = doc.addEntity(std::make_unique<CircleEntity>(Vec2d{50, 0}, 1.0));
    doc.commitTransaction();

    const auto r = measure::minDistance(doc, t, c);
    REQUIRE(r.ok);
    REQUIRE_FALSE(r.exact); // honesty: one side is a bounding box
    REQUIRE_FALSE(r.notes.isEmpty());
}

TEST_CASE("MINDIST command: messages + JSON trailer + overlay", "[mindist][commands]")
{
    Rig rig;
    rig.doc.beginTransaction(QStringLiteral("setup"));
    const EntityId a = addTrace(rig.doc, {0, 0}, {10, 0}, 0.5); // edge y=0.25
    const EntityId b = addTrace(rig.doc, {0, 2}, {10, 2}, 0.3); // edge y=1.85
    rig.doc.commitTransaction();

    const auto res = rig.processor.submit(
        QStringLiteral("MINDIST %1 %2").arg(a).arg(b), /*strict=*/true);
    REQUIRE(res.ok);

    // Human line, closest-points line, JSON trailer.
    const auto& msgs = rig.ctx.messages();
    REQUIRE(msgs.size() >= 3);
    REQUIRE(msgs[0].contains(QStringLiteral("1.6000 mm")));
    REQUIRE(msgs[0].contains(QStringLiteral("(exact)")));

    QJsonObject json;
    for (const QString& m : msgs) {
        const auto d = QJsonDocument::fromJson(m.toUtf8());
        if (d.isObject() && d.object().contains(QStringLiteral("mindist")))
            json = d.object()[QStringLiteral("mindist")].toObject();
    }
    REQUIRE_FALSE(json.isEmpty());
    REQUIRE(json[QStringLiteral("mm")].toDouble() == Approx(1.6).epsilon(1e-9));
    REQUIRE(json[QStringLiteral("overlap")].toBool() == false);
    REQUIRE(json[QStringLiteral("method")].toString() == QStringLiteral("exact"));

    // The witness overlay is armed (line + 2 ticks = 5 strokes)...
    REQUIRE(rig.ctx.overlay().strokes.size() == 5);
    // ...and a subsequent command clears it.
    REQUIRE(rig.processor.submit(QStringLiteral("ID 0,0"), true).ok);
    REQUIRE(rig.ctx.overlay().strokes.empty());
}

TEST_CASE("MINDIST command: a pre-existing selection is honored", "[mindist][commands]")
{
    Rig rig;
    rig.doc.beginTransaction(QStringLiteral("setup"));
    // Disks r=1 and r=2, centers 10 mm apart: 10 - 1 - 2 = 7 mm.
    const EntityId a =
        rig.doc.addEntity(std::make_unique<CircleEntity>(Vec2d{0, 0}, 1.0));
    const EntityId b =
        rig.doc.addEntity(std::make_unique<CircleEntity>(Vec2d{10, 0}, 2.0));
    rig.doc.commitTransaction();

    rig.selection.add(a);
    rig.selection.add(b);
    REQUIRE(rig.processor.submit(QStringLiteral("MINDIST"), true).ok);
    bool found = false;
    for (const QString& m : rig.ctx.messages())
        found = found || m.contains(QStringLiteral("7.0000 mm"));
    REQUIRE(found);
}

TEST_CASE("MINDIST command: bad ids fail politely", "[mindist][commands]")
{
    Rig rig;
    rig.doc.beginTransaction(QStringLiteral("setup"));
    const EntityId a =
        rig.doc.addEntity(std::make_unique<CircleEntity>(Vec2d{0, 0}, 1.0));
    rig.doc.commitTransaction();

    REQUIRE(rig.processor.submit(QStringLiteral("MINDIST %1 999").arg(a), true).ok);
    bool complained = false;
    for (const QString& m : rig.ctx.messages())
        complained = complained || m.contains(QStringLiteral("no entity #999"));
    REQUIRE(complained);
}
