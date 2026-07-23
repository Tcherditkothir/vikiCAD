#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "cmd/CommandProcessor.h"
#include "doc/EntitiesEx.h"

// Command-line ergonomics (usage fixes 2026-07-23):
//  - unique-prefix resolution + the AutoCAD alias table (REC -> RECT...);
//  - an AMBIGUOUS prefix runs NOTHING and lists the candidates;
//  - RECT [Dimensions]: Length/Height with the first corner before or after,
//    negative values growing to the other side (AutoCAD RECTANG semantics).
// Every expected vertex below is derived by hand.

using namespace viki;
using Catch::Approx;

namespace {

struct Rig {
    Document doc;
    SelectionSet selection;
    CommandContext ctx{doc, selection};
    CommandProcessor processor{ctx};
    Rig() { registerBuiltinCommands(processor); }
    CommandProcessor::Result run(const QString& line)
    {
        return processor.submit(line, /*strict=*/true);
    }
};

const PolylineEntity* lastPolyline(const Document& doc)
{
    const PolylineEntity* found = nullptr;
    for (const EntityId id : doc.drawOrder())
        if (const auto* pl = dynamic_cast<const PolylineEntity*>(doc.entity(id)))
            found = pl;
    return found;
}

void checkVerts(const PolylineEntity* pl, std::initializer_list<Vec2d> expected)
{
    REQUIRE(pl != nullptr);
    REQUIRE(pl->isClosed());
    REQUIRE(pl->vertices().size() == expected.size());
    size_t i = 0;
    for (const Vec2d& e : expected) {
        CHECK(pl->vertices()[i].pos.x == Approx(e.x).margin(1e-9));
        CHECK(pl->vertices()[i].pos.y == Approx(e.y).margin(1e-9));
        CHECK(pl->vertices()[i].bulge == 0.0);
        ++i;
    }
}

} // namespace

TEST_CASE("unique-prefix resolution runs exactly one command", "[cmdresolve]")
{
    Rig rig;

    SECTION("REC resolves to RECT and draws (RECT/RECTANGLE/REC are ONE command)")
    {
        REQUIRE(rig.processor.resolveName(QStringLiteral("REC")) ==
                QStringLiteral("RECT"));
        REQUIRE(rig.run(QStringLiteral("REC 0,0 10,10")).ok);
        checkVerts(lastPolyline(rig.doc), {{0, 0}, {10, 0}, {10, 10}, {0, 10}});
    }

    SECTION("longer unique prefixes resolve too")
    {
        CHECK(rig.processor.resolveName(QStringLiteral("DESCR")) ==
              QStringLiteral("DESCRIBE"));
        CHECK(rig.processor.resolveName(QStringLiteral("PANEL")) ==
              QStringLiteral("PANELIZE"));
        CHECK(rig.processor.resolveName(QStringLiteral("rec")) ==
              QStringLiteral("RECT")); // case-insensitive like everything else
    }

    SECTION("ambiguous prefix refuses and LISTS the candidates")
    {
        QString error;
        CHECK(rig.processor.resolveName(QStringLiteral("R"), &error).isEmpty());
        CHECK(error.contains(QStringLiteral("ambiguous")));
        for (const char* cand :
             {"RECT", "REDO", "REVOLVE", "ROTATE", "ROTATE3D"})
            CHECK(error.contains(QLatin1String(cand)));

        const auto r = rig.run(QStringLiteral("R 0,0 10,10"));
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("ambiguous")));
        CHECK(rig.doc.entityCount() == 0); // nothing was launched
    }

    SECTION("exact names and aliases beat prefix ambiguity")
    {
        // EXT is EXTRUDE's registered alias even though the prefix EX*
        // also matches EXTEND/EXPLODE.
        CHECK(rig.processor.resolveName(QStringLiteral("EXT")) ==
              QStringLiteral("EXTRUDE"));
        // SHELL is a command name even though SHELLOPEN extends it.
        CHECK(rig.processor.resolveName(QStringLiteral("SHELL")) ==
              QStringLiteral("SHELL"));
    }

    SECTION("unknown text still reports unknown")
    {
        QString error;
        CHECK(rig.processor.resolveName(QStringLiteral("QQQ"), &error).isEmpty());
        CHECK(error.contains(QStringLiteral("unknown command")));
    }
}

TEST_CASE("AutoCAD alias table is complete and conflict-free", "[cmdresolve]")
{
    Rig rig;

    // The muscle-memory table (nanoCAD/AutoCAD): alias -> canonical command.
    const std::vector<std::pair<QString, QString>> table = {
        {QStringLiteral("REC"), QStringLiteral("RECT")},
        {QStringLiteral("L"), QStringLiteral("LINE")},
        {QStringLiteral("C"), QStringLiteral("CIRCLE")},
        {QStringLiteral("A"), QStringLiteral("ARC")},
        {QStringLiteral("PL"), QStringLiteral("PLINE")},
        {QStringLiteral("M"), QStringLiteral("MOVE")},
        {QStringLiteral("CO"), QStringLiteral("COPY")},
        {QStringLiteral("CP"), QStringLiteral("COPY")},
        {QStringLiteral("RO"), QStringLiteral("ROTATE")},
        {QStringLiteral("MI"), QStringLiteral("MIRROR")},
        {QStringLiteral("SC"), QStringLiteral("SCALE")},
        {QStringLiteral("TR"), QStringLiteral("TRIM")},
        {QStringLiteral("EX"), QStringLiteral("EXTEND")},
        {QStringLiteral("O"), QStringLiteral("OFFSET")},
        {QStringLiteral("F"), QStringLiteral("FILLET")},
        {QStringLiteral("CHA"), QStringLiteral("CHAMFER")},
        {QStringLiteral("E"), QStringLiteral("ERASE")},
        {QStringLiteral("X"), QStringLiteral("EXPLODE")},
        {QStringLiteral("Z"), QStringLiteral("ZOOM")},
        {QStringLiteral("U"), QStringLiteral("UNDO")},
    };
    for (const auto& [alias, target] : table) {
        INFO(alias.toStdString() << " -> " << target.toStdString());
        CHECK(rig.processor.resolveName(alias) == target);
    }

    // Registry DUMP check: no alias may mask a command's own name. If one
    // ever overwrote a canonical entry, resolving that canonical name would
    // come back as a DIFFERENT command.
    QStringList canonicals;
    for (const QString& key : rig.processor.commandNames()) {
        const QString canonical = rig.processor.resolveName(key);
        REQUIRE_FALSE(canonical.isEmpty());
        if (!canonicals.contains(canonical))
            canonicals.push_back(canonical);
    }
    for (const QString& name : canonicals) {
        INFO(name.toStdString());
        CHECK(rig.processor.resolveName(name) == name);
    }

    // Alias rows self-describe in the completion entries ("REC → RECT").
    const QStringList entries = rig.processor.completionEntries();
    CHECK(entries.contains(QStringLiteral("REC → RECT")));
    CHECK(entries.contains(QStringLiteral("L → LINE")));
    CHECK(entries.contains(QStringLiteral("RECT")));
}

TEST_CASE("RECT Dimensions mode — all four combinations by hand", "[rect]")
{
    Rig rig;

    SECTION("D first, positive: RECT D 30 20 5,5")
    {
        REQUIRE(rig.run(QStringLiteral("RECT D 30 20 5,5")).ok);
        checkVerts(lastPolyline(rig.doc), {{5, 5}, {35, 5}, {35, 25}, {5, 25}});
    }

    SECTION("corner first, positive: RECT 5,5 D 30 20")
    {
        REQUIRE(rig.run(QStringLiteral("RECT 5,5 D 30 20")).ok);
        checkVerts(lastPolyline(rig.doc), {{5, 5}, {35, 5}, {35, 25}, {5, 25}});
    }

    SECTION("D first, negative length grows to the other side")
    {
        REQUIRE(rig.run(QStringLiteral("RECT D -30 20 5,5")).ok);
        checkVerts(lastPolyline(rig.doc), {{5, 5}, {-25, 5}, {-25, 25}, {5, 25}});
    }

    SECTION("corner first, negative height grows downward")
    {
        REQUIRE(rig.run(QStringLiteral("RECT 5,5 D 30 -20")).ok);
        checkVerts(lastPolyline(rig.doc), {{5, 5}, {35, 5}, {35, -15}, {5, -15}});
    }

    SECTION("DIMENSIONS long form works like D")
    {
        REQUIRE(rig.run(QStringLiteral("RECT DIMENSIONS 10 4 0,0")).ok);
        checkVerts(lastPolyline(rig.doc), {{0, 0}, {10, 0}, {10, 4}, {0, 4}});
    }

    SECTION("two-corner path untouched, @relative included")
    {
        REQUIRE(rig.run(QStringLiteral("RECT 1,2 11,7")).ok);
        checkVerts(lastPolyline(rig.doc), {{1, 2}, {11, 2}, {11, 7}, {1, 7}});
        REQUIRE(rig.run(QStringLiteral("RECT 20,0 @10,5")).ok);
        checkVerts(lastPolyline(rig.doc), {{20, 0}, {30, 0}, {30, 5}, {20, 5}});
        CHECK(rig.doc.entityCount() == 2);
    }

    SECTION("zero dimension is refused, command keeps prompting")
    {
        // Strict mode: 0 re-prompts for Length, then the implicit Finishes
        // cancel the command — no entity, no error.
        const auto r = rig.run(QStringLiteral("RECT D 0"));
        CHECK(r.ok);
        CHECK(rig.doc.entityCount() == 0);
    }
}
