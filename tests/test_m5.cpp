#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <QFile>
#include <QTemporaryDir>

#include "cmd/CommandProcessor.h"
#include "doc/Annotations.h"
#include "doc/ArrayEntity.h"
#include "doc/Block.h"
#include "doc/Entities.h"
#include "doc/StickyNote.h"
#include "io/NativeStore.h"
#include "io/PdfPlotter.h"
#include "io/QueryJson.h"
#include "script/ScriptRunner.h"

#ifdef VIKICAD_HAS_DXF
#include "io/DxfExporter.h"
#include "io/DxfImporter.h"
#endif

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

TEST_CASE("BLOCK + INSERT + EXPLODE round-trip", "[m5][blocks]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("CIRCLE 5,5 2")));
    REQUIRE(rig.run(QStringLiteral("LINE 0,0 10,0")));
    REQUIRE(rig.run(QStringLiteral("BLOCK BOLT 0,0 1 2")));
    // Originals consumed, one insert added.
    REQUIRE(rig.doc.entityCount() == 1);
    REQUIRE(rig.doc.blockByName(QStringLiteral("BOLT")));
    REQUIRE(rig.doc.blockByName(QStringLiteral("BOLT"))->entities.size() == 2);

    REQUIRE(rig.run(QStringLiteral("INSERT BOLT 100,50 2 90")));
    const auto* ins = dynamic_cast<const InsertEntity*>(rig.doc.entity(4));
    REQUIRE(ins);
    REQUIRE(ins->scale == Approx(2.0));

    // Real bounds via the document (definition expanded, scaled, rotated).
    const BBox2d b = rig.doc.entityBounds(*ins);
    REQUIRE(b.isValid());
    REQUIRE(b.width() > 1.0);

    // Insert renders its definition.
    const auto prims = rig.render(4);
    REQUIRE(prims.strokes.size() >= 2);

    // Explode materializes transformed clones.
    REQUIRE(rig.run(QStringLiteral("EXPLODE 4")));
    REQUIRE(rig.doc.entityCount() == 3); // first insert + 2 pieces
    // The line (0,0)-(10,0) rotated 90° scaled 2 from base 0,0 at 100,50:
    bool found = false;
    for (const EntityId id : rig.doc.drawOrder())
        if (const auto* l = dynamic_cast<const LineEntity*>(rig.doc.entity(id)))
            if (nearEqual(l->p1(), Vec2d{100, 50}, 1e-6) &&
                nearEqual(l->p2(), Vec2d{100, 70}, 1e-6))
                found = true;
    REQUIRE(found);
}

TEST_CASE("block attributes: INSERT values render, EXPLODE makes text", "[m5][blocks]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("ATTDEF TAG DEFVAL 5,5 3")));
    REQUIRE(rig.run(QStringLiteral("BLOCK STAMP 0,0 1")));
    // Scripted insert supplying the attribute value.
    const auto r = runScript(rig.processor,
                             QStringLiteral("INSERT STAMP 50,50 1 0\nACME-42\n"));
    REQUIRE(r.ok);
    EntityId insId = rig.doc.drawOrder().back();
    const auto prims = rig.render(insId);
    REQUIRE(prims.texts.size() == 1);
    REQUIRE(prims.texts[0].text == QStringLiteral("ACME-42"));

    REQUIRE(rig.processor.submit(QStringLiteral("EXPLODE %1").arg(insId), true).ok);
    bool foundText = false;
    for (const EntityId id : rig.doc.drawOrder())
        if (const auto* t = dynamic_cast<const TextEntity*>(rig.doc.entity(id)))
            foundText = foundText || t->text() == QStringLiteral("ACME-42");
    REQUIRE(foundText);
}

TEST_CASE("rectangular and polar arrays regenerate and stay editable", "[m5][array]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("CIRCLE 0,0 1")));
    REQUIRE(rig.run(QStringLiteral("ARRAYRECT 2 3 10 20 1")));
    REQUIRE(rig.doc.entityCount() == 1);
    const auto* arr = dynamic_cast<const ArrayEntity*>(rig.doc.entity(2));
    REQUIRE(arr);
    REQUIRE(arr->itemCount() == 6);
    const BBox2d b = rig.doc.entityBounds(*arr);
    REQUIRE(b.width() == Approx(22.0));  // 2 cols * 10 + 2*r
    REQUIRE(b.height() == Approx(22.0)); // 2 rows * 20 + 2*r

    // Edit the array live: 4 columns.
    REQUIRE(rig.run(QStringLiteral("ARRAYEDIT 1,0 COLS 4")));
    REQUIRE(rig.doc.entityBounds(*arr).width() == Approx(32.0));

    // Suppress an item.
    REQUIRE(rig.run(QStringLiteral("ARRAYEDIT 1,0 SUPPRESS 0")));
    REQUIRE(arr->suppressed.count(0) == 1);

    // Undo restores the previous parameters (journaled edit).
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    REQUIRE(arr->suppressed.empty());

    // Explode materializes 8 circles.
    REQUIRE(rig.run(QStringLiteral("EXPLODE 2")));
    REQUIRE(rig.doc.entityCount() == 8);
}

TEST_CASE("polar array distributes items on the circle", "[m5][array]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("CIRCLE 20,0 2")));
    REQUIRE(rig.run(QStringLiteral("ARRAYPOLAR 0,0 4 360 1")));
    const auto* arr = dynamic_cast<const ArrayEntity*>(rig.doc.entity(2));
    REQUIRE(arr);
    const auto items = arr->materialize();
    REQUIRE(items.size() == 4);
    // Second item rotated 90°: center at (0,20).
    const auto* c2 = dynamic_cast<const CircleEntity*>(items[1].get());
    REQUIRE(c2->center().x == Approx(0.0).margin(1e-9));
    REQUIRE(c2->center().y == Approx(20.0));
}

TEST_CASE("sticky notes: create, pin, query, metadata", "[m5][notes]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("CIRCLE 50,50 10")));
    const auto r = runScript(
        rig.processor,
        QStringLiteral("NOTE 10,10 Verifier **cette** cote\nNOTEPIN 50,60 Pinned note\n"));
    REQUIRE(r.ok);
    REQUIRE(rig.doc.entityCount() == 3);

    const auto notes = queryjson::notesJson(rig.doc);
    REQUIRE(notes.size() == 2);
    REQUIRE(notes[0].toObject()[QStringLiteral("text")].toString().contains(
        QStringLiteral("**cette**")));
    REQUIRE(!notes[0].toObject()[QStringLiteral("author")].toString().isEmpty());
    REQUIRE(!notes[0].toObject()[QStringLiteral("created")].toString().isEmpty());
    // The pinned note follows its target.
    const QJsonObject pinned = notes[1].toObject();
    REQUIRE(pinned.contains(QStringLiteral("target")));

    // Notes layer exists and is not printable.
    Layer* notesLayer = rig.doc.layerByName(QLatin1String(StickyNoteEntity::kLayerName));
    REQUIRE(notesLayer);
    REQUIRE_FALSE(notesLayer->printable);
}

TEST_CASE("native store round-trips blocks, arrays, notes and layouts", "[m5][store]")
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("m5.vkd"));
    {
        Rig rig;
        REQUIRE(rig.run(QStringLiteral("CIRCLE 5,5 2")));
        REQUIRE(rig.run(QStringLiteral("BLOCK B1 0,0 1")));
        REQUIRE(rig.run(QStringLiteral("INSERT B1 30,0 1 0")));
        REQUIRE(rig.run(QStringLiteral("CIRCLE 0,0 1")));
        REQUIRE(rig.run(QStringLiteral("ARRAYRECT 2 2 10 10 4")));
        const auto rs = runScript(rig.processor, QStringLiteral("NOTE 1,1 hello\n"));
        REQUIRE(rs.ok);
        REQUIRE(rig.run(QStringLiteral("LAYOUT SHEET A4L FIT")));
        QString error;
        REQUIRE(NativeStore::save(rig.doc, path, error));
    }
    QString error;
    const auto doc = NativeStore::load(path, error);
    REQUIRE(doc);
    REQUIRE(doc->blockByName(QStringLiteral("B1")));
    REQUIRE(doc->blockByName(QStringLiteral("B1"))->entities.size() == 1);
    REQUIRE(doc->layoutByName(QStringLiteral("SHEET")));
    REQUIRE(doc->layoutByName(QStringLiteral("SHEET"))->viewports.size() == 1);
    int arrays = 0, notes = 0, inserts = 0;
    for (const EntityId id : doc->drawOrder()) {
        arrays += dynamic_cast<const ArrayEntity*>(doc->entity(id)) ? 1 : 0;
        notes += dynamic_cast<const StickyNoteEntity*>(doc->entity(id)) ? 1 : 0;
        inserts += dynamic_cast<const InsertEntity*>(doc->entity(id)) ? 1 : 0;
    }
    REQUIRE(arrays == 1);
    REQUIRE(notes == 1);
    REQUIRE(inserts == 2); // BLOCK's auto-insert + INSERT
    // The array still regenerates after load.
    for (const EntityId id : doc->drawOrder())
        if (const auto* arr = dynamic_cast<const ArrayEntity*>(doc->entity(id)))
            REQUIRE(arr->materialize().size() == 4);
}

TEST_CASE("PDF plot writes a non-trivial file at exact scale", "[m5][pdf]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("LINE 0,0 100,0")));
    REQUIRE(rig.run(QStringLiteral("LAYOUT SHEET A4L 1")));
    const auto rs = runScript(rig.processor, QStringLiteral("NOTE 5,5 dont print me\n"));
    REQUIRE(rs.ok);

    QTemporaryDir dir;
    const QString pdf = dir.filePath(QStringLiteral("out.pdf"));
    QString error;
    const Layout* layout = rig.doc.layoutByName(QStringLiteral("SHEET"));
    REQUIRE(layout);
    REQUIRE(layout->viewports[0].scale == Approx(1.0));
    REQUIRE(plotToPdf(rig.doc, *layout, pdf, error));
    QFile f(pdf);
    REQUIRE(f.exists());
    REQUIRE(f.size() > 1000); // real content, not an empty page
}

#ifdef VIKICAD_HAS_DXF
TEST_CASE("DXF round-trip: blocks, inserts and sticky notes via XDATA", "[m5][dxf]")
{
    QTemporaryDir dir;
    const QString dxf = dir.filePath(QStringLiteral("m5.dxf"));

    Rig rig;
    REQUIRE(rig.run(QStringLiteral("CIRCLE 5,5 2")));
    REQUIRE(rig.run(QStringLiteral("BLOCK BOLT 0,0 1")));
    REQUIRE(rig.run(QStringLiteral("INSERT BOLT 40,0 1.5 45")));
    const auto rs = runScript(
        rig.processor, QStringLiteral("NOTE 10,10 attention au **jeu** fonctionnel\n"));
    REQUIRE(rs.ok);

    const DxfExportResult ex = exportDxf(rig.doc, dxf);
    REQUIRE(ex.ok);

    const DxfImportResult im = importDxf(dxf);
    REQUIRE(im.ok);
    Document& d2 = *im.document;

    // Block definition and inserts round-trip.
    const BlockDef* def = d2.blockByName(QStringLiteral("BOLT"));
    REQUIRE(def);
    REQUIRE(def->entities.size() == 1);
    int inserts = 0;
    for (const EntityId id : d2.drawOrder())
        if (const auto* ins = dynamic_cast<const InsertEntity*>(d2.entity(id))) {
            ++inserts;
            if (ins->position.distanceTo({40, 0}) < 1e-6) {
                REQUIRE(ins->scale == Approx(1.5));
                REQUIRE(ins->rotation == Approx(M_PI / 4).margin(1e-6));
            }
        }
    REQUIRE(inserts == 2);

    // The sticky note came back through XDATA with its metadata.
    const auto notes = queryjson::notesJson(d2);
    REQUIRE(notes.size() == 1);
    REQUIRE(notes[0].toObject()[QStringLiteral("text")].toString() ==
            QStringLiteral("attention au **jeu** fonctionnel"));
    REQUIRE(!notes[0].toObject()[QStringLiteral("author")].toString().isEmpty());
}
#endif

#ifdef VIKICAD_HAS_DXF
TEST_CASE("DXF insert rotation/scale conventions survive a round trip",
          "[m5][dxf][angles]")
{
    QTemporaryDir dir;
    const QString dxf = dir.filePath(QStringLiteral("rot.dxf"));

    Rig rig;
    REQUIRE(rig.run(QStringLiteral("LINE 0,0 10,0")));
    REQUIRE(rig.run(QStringLiteral("BLOCK BAR 0,0 1")));
    // 90° rotation, mirrored in Y, scaled 2x.
    {
        auto ins = std::make_unique<InsertEntity>();
        ins->blockName = QStringLiteral("BAR");
        ins->position = {100, 0};
        ins->rotation = M_PI / 2;
        ins->scale = 2.0;
        ins->scaleY = -2.0;
        rig.doc.beginTransaction(QStringLiteral("t"));
        rig.doc.addEntity(std::move(ins));
        rig.doc.commitTransaction();
    }
    REQUIRE(exportDxf(rig.doc, dxf).ok);
    const DxfImportResult im = importDxf(dxf);
    REQUIRE(im.ok);
    bool checked = false;
    for (const EntityId id : im.document->drawOrder()) {
        const auto* ins = dynamic_cast<const InsertEntity*>(im.document->entity(id));
        if (!ins || ins->position.distanceTo({100, 0}) > 1e-6)
            continue;
        checked = true;
        REQUIRE(ins->rotation == Approx(M_PI / 2).margin(1e-6));
        REQUIRE(ins->scale == Approx(2.0));
        REQUIRE(ins->effScaleY() == Approx(-2.0));
        // The transformed line end lands where the math says: base (0,0),
        // p2 (10,0) → rot 90° scale(2,-2) → (0,20) + insert (100,0).
        const BBox2d b = im.document->entityBounds(*ins);
        REQUIRE(b.max.y == Approx(20.0).margin(1e-6));
    }
    REQUIRE(checked);
}
#endif
