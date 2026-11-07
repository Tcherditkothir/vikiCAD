// SETCOLOR (issue #3) — first-class per-entity colour + transparency command:
//   - <id|selection> <#RRGGBB|named> [alpha], one undoable transaction;
//   - pickfirst: SELECT ids then a bare `SETCOLOR` targets the selection;
//   - a bad colour refuses with a message that shows the accepted forms;
//   - the colour survives a .vkd round-trip and styles a STEP export.
// Plus the issue-#2 guard: the EntitySet "expected entity ids" error names
// the command still waiting, so a chained .vks line names the culprit.
//
// Every expected value below is hand-derived (a 40x30x10 box).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <QFile>
#include <QTemporaryDir>

#include "cmd/CommandProcessor.h"
#include "doc/Document.h"
#include "doc/SelectionSet.h"
#include "io/NativeStore.h"
#include "io/StepIo.h"
#include "script/ScriptRunner.h"
#include "solid/SolidEntity.h"

using namespace viki;
using Catch::Approx;

namespace {

struct Rig {
    Document doc;
    SelectionSet selection;
    CommandContext ctx{doc, selection};
    CommandProcessor processor{ctx};
    Rig() { registerBuiltinCommands(processor); }
    CommandProcessor::Result run(const QString& s)
    {
        return processor.submit(s, /*strict=*/true);
    }
    bool ok(const QString& s) { return run(s).ok; }
};

// A 40x30x10 box: RECT (id 1) consumed by EXTRUDE -> solid id 2.
void buildBox(Rig& rig)
{
    REQUIRE(rig.ok(QStringLiteral("RECT 0,0 40,30")));
    REQUIRE(rig.ok(QStringLiteral("EXTRUDE 10 1")));
}

const SolidEntity* soleSolid(const Document& doc)
{
    const SolidEntity* found = nullptr;
    for (const EntityId id : doc.drawOrder())
        if (const auto* s = dynamic_cast<const SolidEntity*>(doc.entity(id)))
            found = s;
    return found;
}

} // namespace

TEST_CASE("SETCOLOR sets the explicit colour and alpha of a solid", "[setcolor]")
{
    Rig rig;
    buildBox(rig);

    SECTION("hex colour, one message, colour stored")
    {
        REQUIRE(rig.ok(QStringLiteral("SETCOLOR 2 #E8AD23")));
        const SolidEntity* s = soleSolid(rig.doc);
        REQUIRE(s);
        CHECK_FALSE(s->color().byLayer);
        CHECK(s->color().rgb == 0xE8AD23u);
        bool said = false;
        for (const QString& m : rig.ctx.messages())
            said = said || m.contains(QStringLiteral("recoloured"));
        CHECK(said);
    }

    SECTION("named colour resolves to its RGB")
    {
        REQUIRE(rig.ok(QStringLiteral("SETCOLOR 2 red")));
        CHECK(soleSolid(rig.doc)->color().rgb == 0xFF0000u);
    }

    SECTION("alpha clamps to [0,100] and feeds transparency")
    {
        REQUIRE(rig.ok(QStringLiteral("SETCOLOR 2 blue 30")));
        const SolidEntity* s = soleSolid(rig.doc);
        CHECK(s->color().rgb == 0x0000FFu);
        CHECK(s->transparency == Approx(0.30).margin(1e-9));

        // 101 clamps to 100 (fully transparent), 0 back to opaque.
        REQUIRE(rig.ok(QStringLiteral("SETCOLOR 2 blue 101")));
        CHECK(soleSolid(rig.doc)->transparency == Approx(1.0).margin(1e-9));
        REQUIRE(rig.ok(QStringLiteral("SETCOLOR 2 blue 0")));
        CHECK(soleSolid(rig.doc)->transparency == Approx(0.0).margin(1e-9));
    }

    SECTION("pickfirst: SELECT 2 then bare SETCOLOR")
    {
        REQUIRE(rig.ok(QStringLiteral("SELECT 2")));
        REQUIRE(rig.ok(QStringLiteral("SETCOLOR green")));
        CHECK(soleSolid(rig.doc)->color().rgb == 0x00FF00u);
    }

    SECTION("bad colour refuses with the accepted forms, nothing changed")
    {
        // Cancellation surfaces as ok:true with the explanation in messages
        // (the documented ok:true-doesn't-mean-did-something contract).
        const auto r = rig.run(QStringLiteral("SETCOLOR 2 notacolor"));
        bool explained = false;
        for (const QString& m : rig.ctx.messages())
            explained = explained || m.contains(QStringLiteral("RRGGBB"));
        CHECK(explained);
        CHECK(soleSolid(rig.doc)->color().byLayer); // untouched
        CHECK(r.ok); // cancelled, not an input-parse failure
    }

    SECTION("one UNDO puts the colour back")
    {
        REQUIRE(rig.ok(QStringLiteral("SETCOLOR 2 #112233 40")));
        REQUIRE(rig.ok(QStringLiteral("UNDO")));
        const SolidEntity* s = soleSolid(rig.doc);
        CHECK(s->color().byLayer);
        CHECK(s->transparency == Approx(0.0).margin(1e-9));
    }

    SECTION("2D entities take the colour (no alpha channel)")
    {
        REQUIRE(rig.ok(QStringLiteral("RECT 100,0 110,10"))); // entity 3
        REQUIRE(rig.ok(QStringLiteral("SETCOLOR 3 orange")));
        const Entity* e = rig.doc.entity(3);
        REQUIRE(e);
        CHECK_FALSE(e->color().byLayer);
        CHECK(e->color().rgb == 0xFFA500u);
    }
}

TEST_CASE("SETCOLOR colour and alpha survive .vkd and STEP round-trips",
          "[setcolor]")
{
    Rig rig;
    buildBox(rig);
    REQUIRE(rig.ok(QStringLiteral("SETCOLOR 2 #E8AD23 25")));

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString vkd = dir.filePath(QStringLiteral("c.vkd"));
    QString saveError;
    REQUIRE(NativeStore::save(rig.doc, vkd, saveError));

    QString error;
    const auto back = NativeStore::load(vkd, error);
    REQUIRE(back);
    const auto* s = dynamic_cast<const SolidEntity*>(back->entity(2));
    REQUIRE(s);
    CHECK_FALSE(s->color().byLayer);
    CHECK(s->color().rgb == 0xE8AD23u);
    CHECK(s->transparency == Approx(0.25).margin(1e-9));

    // And the colour is what the STEP writer styles from — the same field
    // exportStep honours (verified by test_step_color; here just the file).
    const QString step = dir.filePath(QStringLiteral("c.step"));
    const StepResult w = exportStep(*back, step);
    REQUIRE(w.ok);
    QFile f(step);
    REQUIRE(f.open(QIODevice::ReadOnly));
    CHECK(f.readAll().contains("COLOUR_RGB"));
}

TEST_CASE("EntitySet refusal names the command still waiting (issue #2)",
          "[setcolor][cmderrors]")
{
    Rig rig;
    REQUIRE(rig.ok(QStringLiteral("RECT 0,0 40,30")));
    // Script semantics (non-strict, like a .vks line): "EXTRUDE 10 1" — "1"
    // lands at the MODE prompt (digits = legacy profile), the command parks
    // at "Select profiles:" and STAYS PENDING. The next line's verb ("UNION")
    // is then fed into that pending EntitySet: the error must NAME the
    // waiting command instead of a bare "expected entity ids".
    REQUIRE(rig.processor.submit(QStringLiteral("EXTRUDE 10 1"), /*strict=*/false)
                .pending);
    const auto r = rig.processor.submit(QStringLiteral("UNION 2 2"),
                                        /*strict=*/false);
    CHECK_FALSE(r.ok);
    CHECK(r.error.contains(QStringLiteral("expected entity ids")));
    CHECK(r.error.contains(QStringLiteral("EXTRUDE")));
    CHECK(r.error.contains(QStringLiteral("Select profiles")));
}