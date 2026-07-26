#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "cmd/CommandProcessor.h"
#include "doc/Annotations.h"
#include "doc/Block.h"
#include "doc/Document.h"
#include "doc/Entities.h"
#include "doc/SelectionSet.h"
#include "io/ClipboardIo.h"
#include "solid/SolidEntity.h"
#include "solid/SolidMetrics.h"

using namespace viki;
using Catch::Matchers::WithinAbs;

namespace {
struct Rig {
    Document doc;
    SelectionSet selection;
    CommandContext ctx{doc, selection};
    CommandProcessor processor{ctx};
    // No clipboard hook: the commands share the process-local buffer, which
    // is exactly how two documents of one test process exchange data.
    Rig() { registerBuiltinCommands(processor); }
    bool run(const QString& s) { return processor.submit(s, true).ok; }
};

EntityId lastId(const Document& doc)
{
    EntityId last = kInvalidEntityId;
    for (const EntityId id : doc.drawOrder())
        last = std::max(last, id);
    return last;
}

// A 10x10x10 box at (x0,y0) — EXTRUDE's second argument is the PROFILE ID.
SolidEntity* buildBox(Rig& rig, double x0 = 0.0, double y0 = 0.0)
{
    REQUIRE(rig.run(QStringLiteral("RECT %1,%2 %3,%4")
                        .arg(x0)
                        .arg(y0)
                        .arg(x0 + 10.0)
                        .arg(y0 + 10.0)));
    const EntityId rectId = lastId(rig.doc);
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 %1").arg(rectId)));

    SolidEntity* last = nullptr;
    for (const EntityId id : rig.doc.drawOrder())
        if (auto* s = dynamic_cast<SolidEntity*>(rig.doc.entity(id)))
            last = s;
    return last;
}
} // namespace

TEST_CASE("COPYCLIP + PASTECLIP duplicates within a document", "[clipboard]")
{
    processClipboardBuffer().clear();
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 10,10")));
    const EntityId rectId = lastId(rig.doc);

    REQUIRE(rig.run(QStringLiteral("COPYCLIP %1").arg(rectId)));
    REQUIRE(rig.run(QStringLiteral("PASTECLIP 30,0")));

    REQUIRE(rig.doc.entityCount() == 2);
    // The copied set's anchor is its lower-left corner (0,0), so the paste
    // lands the duplicate exactly at 30,0.
    const BBox2d ext = rig.doc.extents();
    CHECK_THAT(ext.max.x, WithinAbs(40.0, 1e-9));
    CHECK_THAT(ext.max.y, WithinAbs(10.0, 1e-9));

    // The pasted entity is selected and is a NEW id.
    const EntityId pastedId = lastId(rig.doc);
    CHECK(pastedId != rectId);
    REQUIRE(rig.selection.size() == 1);
    CHECK(rig.selection.ids().front() == pastedId);

    // One UNDO removes the paste, the original stays.
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    CHECK(rig.doc.entityCount() == 1);
    CHECK(rig.doc.entity(rectId) != nullptr);
}

TEST_CASE("PASTECLIP without a point pastes at the original position",
          "[clipboard]")
{
    processClipboardBuffer().clear();
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("RECT 5,5 15,25")));
    const EntityId rectId = lastId(rig.doc);
    const BBox2d before = rig.doc.entityBounds(*rig.doc.entity(rectId));

    REQUIRE(rig.run(QStringLiteral("COPYCLIP %1").arg(rectId)));
    // Strict mode's implicit Finish = the GUI's Enter at the insertion
    // prompt: paste in place (the cross-document alignment case).
    REQUIRE(rig.run(QStringLiteral("PASTECLIP")));

    REQUIRE(rig.doc.entityCount() == 2);
    const BBox2d after = rig.doc.entityBounds(*rig.doc.entity(lastId(rig.doc)));
    CHECK_THAT(after.min.x, WithinAbs(before.min.x, 1e-9));
    CHECK_THAT(after.min.y, WithinAbs(before.min.y, 1e-9));
    CHECK_THAT(after.max.x, WithinAbs(before.max.x, 1e-9));
    CHECK_THAT(after.max.y, WithinAbs(before.max.y, 1e-9));
}

TEST_CASE("the clipboard crosses documents and recreates layers by name",
          "[clipboard]")
{
    processClipboardBuffer().clear();
    Rig src;
    const LayerId tole = src.doc.ensureLayer(QStringLiteral("TOLE"), 0xFF0000);
    src.doc.setCurrentLayer(tole);
    REQUIRE(src.run(QStringLiteral("RECT 0,0 10,10")));
    REQUIRE(src.run(QStringLiteral("COPYCLIP %1").arg(lastId(src.doc))));

    SECTION("into a document that has no such layer") {
        Rig dst;
        REQUIRE(dst.doc.layerByName(QStringLiteral("TOLE")) == nullptr);
        REQUIRE(dst.run(QStringLiteral("PASTECLIP")));

        REQUIRE(dst.doc.entityCount() == 1);
        const Layer* created = dst.doc.layerByName(QStringLiteral("TOLE"));
        REQUIRE(created != nullptr);
        CHECK(created->rgb == 0xFF0000u); // the carried colour
        CHECK(dst.doc.entity(lastId(dst.doc))->layerId() == created->id);
    }

    SECTION("an existing layer of the same name wins untouched") {
        Rig dst;
        const LayerId existing =
            dst.doc.ensureLayer(QStringLiteral("TOLE"), 0x0000FF);
        const size_t layersBefore = dst.doc.layers().size();
        REQUIRE(dst.run(QStringLiteral("PASTECLIP")));

        CHECK(dst.doc.layers().size() == layersBefore); // no duplicate
        const Layer* kept = dst.doc.layerByName(QStringLiteral("TOLE"));
        REQUIRE(kept != nullptr);
        CHECK(kept->rgb == 0x0000FFu); // target's colour untouched
        CHECK(dst.doc.entity(lastId(dst.doc))->layerId() == existing);
    }
}

TEST_CASE("a solid travels with BREP, colour, transparency and component",
          "[clipboard]")
{
    processClipboardBuffer().clear();
    Rig src;
    SolidEntity* box = buildBox(src);
    REQUIRE(box != nullptr);
    box->setColor(ColorSpec{/*byLayer=*/false, 0xE8AD23});
    box->transparency = 0.25;
    box->component = QStringLiteral("BOUCLIER");
    REQUIRE(src.run(QStringLiteral("COPYCLIP %1").arg(box->id())));

    Rig dst;
    REQUIRE(dst.run(QStringLiteral("PASTECLIP 50,0")));

    REQUIRE(dst.doc.entityCount() == 1);
    const auto* pasted =
        dynamic_cast<const SolidEntity*>(dst.doc.entity(lastId(dst.doc)));
    REQUIRE(pasted != nullptr);
    const auto metrics = solidops::solidMetrics(pasted->shape());
    REQUIRE(metrics.valid);
    CHECK_THAT(metrics.volume, WithinAbs(1000.0, 1e-6)); // the 10 mm box
    // Translated to the insertion point: X spans 50..60 now.
    CHECK_THAT(metrics.bboxMin.X(), WithinAbs(50.0, 1e-6));
    CHECK_THAT(metrics.bboxMax.X(), WithinAbs(60.0, 1e-6));
    CHECK_FALSE(pasted->color().byLayer);
    CHECK(pasted->color().rgb == 0xE8AD23u);
    CHECK_THAT(pasted->transparency, WithinAbs(0.25, 1e-9));
    CHECK(pasted->component == QStringLiteral("BOUCLIER"));
}

TEST_CASE("CUTCLIP removes the originals and UNDO restores them", "[clipboard]")
{
    processClipboardBuffer().clear();
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 10,10")));
    const EntityId rectId = lastId(rig.doc);
    REQUIRE(rig.run(QStringLiteral("CIRCLE 5,5 2")));
    const EntityId circId = lastId(rig.doc);
    REQUIRE(rectId != circId);

    REQUIRE(rig.run(QStringLiteral("CUTCLIP %1 %2").arg(rectId).arg(circId)));
    CHECK(rig.doc.entityCount() == 0);

    // The payload holds both; pasting in place brings them back.
    REQUIRE(rig.run(QStringLiteral("PASTECLIP")));
    CHECK(rig.doc.entityCount() == 2);
    const BBox2d ext = rig.doc.extents();
    CHECK_THAT(ext.min.x, WithinAbs(0.0, 1e-9));
    CHECK_THAT(ext.max.x, WithinAbs(10.0, 1e-9));

    // The cut itself is one journaled transaction.
    REQUIRE(rig.run(QStringLiteral("UNDO"))); // undo the paste
    CHECK(rig.doc.entityCount() == 0);
    REQUIRE(rig.run(QStringLiteral("UNDO"))); // undo the cut
    CHECK(rig.doc.entityCount() == 2);
    CHECK(rig.doc.entity(rectId) != nullptr);
    CHECK(rig.doc.entity(circId) != nullptr);
}

TEST_CASE("foreign or empty clipboard data is refused without touching the "
          "document",
          "[clipboard]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 10,10")));

    SECTION("garbage bytes") {
        processClipboardBuffer() = QByteArrayLiteral("hello, not json");
    }
    SECTION("valid JSON of some other application") {
        processClipboardBuffer() = QByteArrayLiteral("{\"foo\": 1}");
    }
    SECTION("empty clipboard") { processClipboardBuffer().clear(); }

    // The command reports and completes; nothing was added, and UNDO still
    // targets the RECT (no empty transaction was journaled).
    REQUIRE(rig.run(QStringLiteral("PASTECLIP")));
    CHECK(rig.doc.entityCount() == 1);
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    CHECK(rig.doc.entityCount() == 0);
}

TEST_CASE("a foreign token at the insertion prompt pastes in place and starts "
          "the next command",
          "[clipboard]")
{
    processClipboardBuffer().clear();
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 10,10")));
    REQUIRE(rig.run(QStringLiteral("COPYCLIP %1").arg(lastId(rig.doc))));

    // The IPC shape of the trap: a NON-strict "PASTECLIP" stays pending at
    // the insertion prompt; the next line must not be swallowed (.scr
    // semantics — the historic .vks trap).
    const auto pending = rig.processor.submit(QStringLiteral("PASTECLIP"), false);
    REQUIRE(pending.ok);
    REQUIRE(pending.pending);
    const auto next =
        rig.processor.submit(QStringLiteral("RECT 20,20 25,25"), false);
    REQUIRE(next.ok);
    CHECK_FALSE(next.pending);

    // Both happened: the paste (in place) AND the rectangle.
    CHECK(rig.doc.entityCount() == 3);
    const BBox2d ext = rig.doc.extents();
    CHECK_THAT(ext.max.x, WithinAbs(25.0, 1e-9));
}

TEST_CASE("a block insert carries its definition into the target document",
          "[clipboard]")
{
    processClipboardBuffer().clear();
    Rig src;
    BlockDef* def = src.doc.createBlock(QStringLiteral("M3"), Vec2d{0.0, 0.0});
    def->entities.push_back(
        std::make_unique<LineEntity>(Vec2d{0.0, 0.0}, Vec2d{4.0, 0.0}));

    src.doc.beginTransaction(QStringLiteral("test"));
    auto insert = std::make_unique<InsertEntity>();
    insert->blockName = QStringLiteral("M3");
    insert->position = Vec2d{7.0, 3.0};
    const EntityId insertId = src.doc.addEntity(std::move(insert));
    src.doc.commitTransaction();

    REQUIRE(src.run(QStringLiteral("COPYCLIP %1").arg(insertId)));

    Rig dst;
    REQUIRE(dst.doc.blockByName(QStringLiteral("M3")) == nullptr);
    REQUIRE(dst.run(QStringLiteral("PASTECLIP")));

    REQUIRE(dst.doc.entityCount() == 1);
    const BlockDef* carried = dst.doc.blockByName(QStringLiteral("M3"));
    REQUIRE(carried != nullptr);
    CHECK(carried->entities.size() == 1);
    // The insert still resolves: its bounds expand through the definition.
    const Entity* pasted = dst.doc.entity(lastId(dst.doc));
    REQUIRE(pasted != nullptr);
    CHECK(dst.doc.entityBounds(*pasted).isValid());
}

TEST_CASE("a dimension style rides along when the target lacks it", "[clipboard]")
{
    processClipboardBuffer().clear();
    Rig src;
    DimStyle fancy;
    fancy.name = QStringLiteral("Fancy");
    fancy.textHeight = 7.5;
    src.doc.upsertDimStyle(fancy);

    src.doc.beginTransaction(QStringLiteral("test"));
    auto dim = std::make_unique<DimensionEntity>();
    dim->a = Vec2d{0.0, 0.0};
    dim->b = Vec2d{25.0, 0.0};
    dim->pos = Vec2d{12.5, 8.0};
    dim->style = QStringLiteral("Fancy");
    const EntityId dimId = src.doc.addEntity(std::move(dim));
    src.doc.commitTransaction();

    REQUIRE(src.run(QStringLiteral("COPYCLIP %1").arg(dimId)));

    Rig dst;
    REQUIRE(dst.run(QStringLiteral("PASTECLIP")));
    REQUIRE(dst.doc.entityCount() == 1);
    CHECK_THAT(dst.doc.dimStyle(QStringLiteral("Fancy")).textHeight,
               WithinAbs(7.5, 1e-9));
}
