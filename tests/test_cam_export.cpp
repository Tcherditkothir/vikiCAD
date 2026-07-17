// G3 — closing the CAM loop: edit, then SHIP the files.
//
//  - Gerber KIT export (io/GerberKitWriter.h): role+side -> Altium/Protel
//    extension mapping, drills grouped into one .TXT, skipped-layer report;
//  - THE end-to-end CAM scenario on the real kit A: import -> MOVE a pad
//    2 mm -> edit a trace width (PLWIDTH) -> ERASE a silk stroke -> add a
//    drill hole (LAYER Drill CURRENT + CIRCLE) -> export the kit ->
//    re-import it -> every edit must have survived; gerbv then renders the
//    EXPORT and its re-export identically (gerbv vs gerbv, tight
//    thresholds — our renderer stays out of the loop);
//  - PANELIZE: grid duplication of the fab content, one transaction, undo
//    restores; the exported 2x2 panel carries ~4x the ink of the board
//    under gerbv (±5 %);
//  - the DXF<->Gerber bridge: trace widths cross the DXF boundary both
//    ways (LWPOLYLINE code 43), and a closed 2D polyline on an
//    Outline-role layer exports as a clean .GKO.
//
// Real-kit and gerbv tiers SKIP cleanly when the private kits or gerbv are
// absent (same convention as the other gerber suites).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cmath>
#include <vector>

#include "cmd/CommandProcessor.h"
#include "doc/Block.h"
#include "doc/Document.h"
#include "doc/Entities.h"
#include "doc/EntitiesEx.h"
#include "doc/GerberRole.h"
#include "doc/SelectionSet.h"
#include "io/GerberIo.h"
#include "io/GerberKit.h"
#include "io/GerberKitWriter.h"
#ifdef VIKICAD_HAS_DXF
#include "io/DxfExporter.h"
#include "io/DxfImporter.h"
#endif

using namespace viki;
using Catch::Approx;

namespace {

const char* kKitRoot = "/home/lex/computer/pcb-ref";

bool kitAPresent()
{
    return QDir(QLatin1String(kKitRoot) + QLatin1String("/S5M0PCBA")).exists();
}

QString gerbvExe()
{
    return QStandardPaths::findExecutable(QStringLiteral("gerbv"));
}

const Layer* layerNamed(const Document& doc, const char* name)
{
    for (const Layer& l : doc.layers())
        if (l.name == QLatin1String(name))
            return &l;
    return nullptr;
}

std::vector<const Entity*> entitiesOn(const Document& doc, const char* layer)
{
    std::vector<const Entity*> out;
    const Layer* l = layerNamed(doc, layer);
    if (!l)
        return out;
    for (const EntityId id : doc.drawOrder()) {
        const Entity* e = doc.entity(id);
        if (e && e->layerId() == l->id)
            out.push_back(e);
    }
    return out;
}

// Run one command line through a fresh processor bound to `doc`.
void run(Document& doc, const QString& line)
{
    SelectionSet sel;
    CommandContext ctx(doc, sel);
    CommandProcessor proc(ctx);
    registerBuiltinCommands(proc);
    const auto r = proc.submit(line, /*strict=*/true);
    INFO(line.toStdString() << ": " << r.error.toStdString());
    REQUIRE(r.ok);
}

// ---- gerbv comparison helpers (same logic as test_gerber_export.cpp) ------

bool renderWithGerbv(const QString& gerbv, const QString& file, const QString& png)
{
    const QStringList args{QStringLiteral("--export=png"),
                           QStringLiteral("--output=") + png,
                           QStringLiteral("--dpi=400"),
                           QStringLiteral("--border=0"),
                           QStringLiteral("--background=#000000"),
                           QStringLiteral("--foreground=#FFFFFF"),
                           file};
    QProcess p;
    p.start(gerbv, args);
    if (!p.waitForFinished(30000))
        return false;
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0 &&
           QFileInfo(png).size() > 0;
}

struct InkMask {
    QImage img;       // 256x256 grayscale, cropped to ink bbox
    double ink = 0.0; // fraction of bright pixels after normalization
};

InkMask inkMask(const QString& png)
{
    InkMask out;
    QImage src(png);
    if (src.isNull())
        return out;
    QImage gray = src.convertToFormat(QImage::Format_Grayscale8);
    int minX = gray.width(), minY = gray.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < gray.height(); ++y) {
        const uchar* line = gray.constScanLine(y);
        for (int x = 0; x < gray.width(); ++x)
            if (line[x] > 48) {
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
    }
    if (maxX < 0)
        return out;
    QImage mask(gray.width(), gray.height(), QImage::Format_Grayscale8);
    for (int y = 0; y < gray.height(); ++y) {
        const uchar* in = gray.constScanLine(y);
        uchar* dst = mask.scanLine(y);
        for (int x = 0; x < gray.width(); ++x)
            dst[x] = in[x] > 48 ? 255 : 0;
    }
    const QImage cropped = mask.copy(QRect(QPoint(minX, minY), QPoint(maxX, maxY)));
    out.img = cropped.scaled(256, 256, Qt::IgnoreAspectRatio,
                             Qt::SmoothTransformation)
                  .convertToFormat(QImage::Format_Grayscale8);
    int bright = 0;
    for (int y = 0; y < 256; ++y) {
        const uchar* line = out.img.constScanLine(y);
        for (int x = 0; x < 256; ++x)
            if (line[x] > 127)
                ++bright;
    }
    out.ink = double(bright) / (256.0 * 256.0);
    return out;
}

int dhashDistance(const QImage& a, const QImage& b)
{
    constexpr int N = 32;
    const auto bits = [](const QImage& img) {
        const QImage s = img.scaled(N + 1, N, Qt::IgnoreAspectRatio,
                                    Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_Grayscale8);
        std::vector<int> out;
        out.reserve(N * N);
        for (int y = 0; y < N; ++y) {
            const uchar* line = s.constScanLine(y);
            for (int x = 0; x < N; ++x)
                out.push_back(line[x] < line[x + 1] ? 1 : 0);
        }
        return out;
    };
    const auto ba = bits(a), bb = bits(b);
    int d = 0;
    for (size_t i = 0; i < ba.size(); ++i)
        d += ba[i] != bb[i];
    return d;
}

// RAW bright-pixel count (no crop/normalize): at a fixed gerbv DPI the ink
// AREA is scale-independent — a 2x2 panel must carry ~4x the board's ink.
qint64 rawInkCount(const QString& png)
{
    QImage gray = QImage(png).convertToFormat(QImage::Format_Grayscale8);
    qint64 n = 0;
    for (int y = 0; y < gray.height(); ++y) {
        const uchar* line = gray.constScanLine(y);
        for (int x = 0; x < gray.width(); ++x)
            if (line[x] > 48)
                ++n;
    }
    return n;
}

} // namespace

// ---------------------------------------------------------------------------
// Kit extension mapping + synthetic kit export
// ---------------------------------------------------------------------------

TEST_CASE("Kit writer: role+side to extension mapping", "[gerberkit][export]")
{
    Layer l;
    l.name = QStringLiteral("Top-Copper");
    l.gerberRole = QStringLiteral("Copper-Top");
    CHECK(kitExtensionForLayer(l) == QStringLiteral(".GTL"));
    l.name = QStringLiteral("Bottom-Copper");
    l.gerberRole = QStringLiteral("Copper-Bottom");
    CHECK(kitExtensionForLayer(l) == QStringLiteral(".GBL"));
    l.name = QStringLiteral("Top-Mask");
    l.gerberRole = QStringLiteral("Mask");
    CHECK(kitExtensionForLayer(l) == QStringLiteral(".GTS"));
    l.name = QStringLiteral("Bottom-Silk");
    l.gerberRole = QStringLiteral("Silk");
    CHECK(kitExtensionForLayer(l) == QStringLiteral(".GBO"));
    l.name = QStringLiteral("Top-Paste");
    l.gerberRole = QStringLiteral("Paste");
    CHECK(kitExtensionForLayer(l) == QStringLiteral(".GTP"));
    l.name = QStringLiteral("Board");
    l.gerberRole = QStringLiteral("Outline");
    CHECK(kitExtensionForLayer(l) == QStringLiteral(".GKO"));
    l.name = QStringLiteral("Drill");
    l.gerberRole = QStringLiteral("Drill");
    CHECK(kitExtensionForLayer(l) == QStringLiteral(".TXT"));
    // Sideless Mask cannot be mapped; Mech and role-less layers neither.
    l.name = QStringLiteral("SolderMask");
    l.gerberRole = QStringLiteral("Mask");
    CHECK(kitExtensionForLayer(l).isEmpty());
    l.name = QStringLiteral("Mech-15");
    l.gerberRole = QStringLiteral("Mech");
    CHECK(kitExtensionForLayer(l).isEmpty());
    l.name = QStringLiteral("Top-Pads");
    l.gerberRole.clear();
    CHECK(kitExtensionForLayer(l).isEmpty());
}

TEST_CASE("Kit writer: synthetic kit export + skipped layers", "[gerberkit][export]")
{
    Document doc;
    const LayerId copper = doc.ensureLayer(QStringLiteral("Top-Copper"));
    doc.setLayerGerberRole(copper, QStringLiteral("Copper-Top"));
    const LayerId drill = doc.ensureLayer(QStringLiteral("Drill"));
    doc.setLayerGerberRole(drill, QStringLiteral("Drill"));
    const LayerId npth = doc.ensureLayer(QStringLiteral("Drill-NPTH"));
    doc.setLayerGerberRole(npth, QStringLiteral("Drill-NPTH"));
    const LayerId mech = doc.ensureLayer(QStringLiteral("Mech-1"));
    doc.setLayerGerberRole(mech, QStringLiteral("Mech"));
    const LayerId empty = doc.ensureLayer(QStringLiteral("Top-Mask"));
    doc.setLayerGerberRole(empty, QStringLiteral("Mask"));

    {
        TransactionScope tx(doc, QStringLiteral("T"));
        auto pl = std::make_unique<PolylineEntity>(
            std::vector<PolyVertex>{{{0.0, 0.0}, 0.0}, {{10.0, 0.0}, 0.0}}, false);
        pl->setWidth(0.3);
        pl->setLayerId(copper);
        doc.addEntity(std::move(pl));
        auto c1 = std::make_unique<CircleEntity>(Vec2d{2.0, 2.0}, 0.5);
        c1->setLayerId(drill);
        doc.addEntity(std::move(c1));
        auto c2 = std::make_unique<CircleEntity>(Vec2d{8.0, 2.0}, 1.2);
        c2->setLayerId(npth);
        doc.addEntity(std::move(c2));
        auto ml = std::make_unique<LineEntity>(Vec2d{0.0, 0.0}, Vec2d{10.0, 10.0});
        ml->setLayerId(mech);
        doc.addEntity(std::move(ml));
        tx.commit();
    }

    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const GerberKitExportResult r =
        exportGerberKit(doc, tmp.path(), QStringLiteral("brd"));
    INFO(r.error.toStdString());
    REQUIRE(r.ok);
    REQUIRE(r.files.size() == 2); // GTL + one TXT for BOTH drill layers
    CHECK(QFileInfo(tmp.filePath(QStringLiteral("brd.GTL"))).exists());
    CHECK(QFileInfo(tmp.filePath(QStringLiteral("brd.TXT"))).exists());
    const GerberKitExportFile& drillFile = r.files.back();
    CHECK(drillFile.isDrill);
    CHECK(drillFile.entities == 2);
    CHECK(drillFile.layers ==
          QStringList({QStringLiteral("Drill"), QStringLiteral("Drill-NPTH")}));
    // Mech (no kit extension) and the empty mask are reported, not written.
    bool mechSkipped = false, maskSkipped = false;
    for (const QString& s : r.skippedLayers) {
        mechSkipped = mechSkipped || s.startsWith(QLatin1String("Mech-1:"));
        maskSkipped = maskSkipped || (s.startsWith(QLatin1String("Top-Mask:")) &&
                                      s.contains(QLatin1String("empty")));
    }
    CHECK(mechSkipped);
    CHECK(maskSkipped);
    CHECK(!QFileInfo(tmp.filePath(QStringLiteral("brd.GTS"))).exists());

    // layersForKitExtension resolves both spellings.
    CHECK(layersForKitExtension(doc, QStringLiteral("gtl")) ==
          QStringList{QStringLiteral("Top-Copper")});
    CHECK(layersForKitExtension(doc, QStringLiteral(".TXT")) ==
          QStringList({QStringLiteral("Drill"), QStringLiteral("Drill-NPTH")}));

    // A document with no fab role at all refuses loudly.
    Document bare;
    const GerberKitExportResult r2 =
        exportGerberKit(bare, tmp.path(), QStringLiteral("x"));
    CHECK(!r2.ok);
    CHECK(r2.error.contains(QLatin1String("ROLE")));
}

TEST_CASE("Kit writer G3 closure: extension/dialect warnings, hard error "
          "leaves NO partial kit, PANELIZE refuses absurd grids",
          "[gerberkit][export][g3]")
{
    Document doc;
    const LayerId copper = doc.ensureLayer(QStringLiteral("Top-Copper"));
    doc.setLayerGerberRole(copper, QStringLiteral("Copper-Top"));
    const LayerId drill = doc.ensureLayer(QStringLiteral("Drill"));
    doc.setLayerGerberRole(drill, QStringLiteral("Drill"));
    {
        TransactionScope tx(doc, QStringLiteral("T"));
        auto pl = std::make_unique<PolylineEntity>(
            std::vector<PolyVertex>{{{0.0, 0.0}, 0.0}, {{10.0, 0.0}, 0.0}}, false);
        pl->setWidth(0.3);
        pl->setLayerId(copper);
        doc.addEntity(std::move(pl));
        auto c = std::make_unique<CircleEntity>(Vec2d{2.0, 2.0}, 0.5);
        c->setLayerId(drill);
        doc.addEntity(std::move(c));
        tx.commit();
    }
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());

    // The role picks the dialect, but a contradicting EXTENSION now warns:
    // fab houses sort files by extension.
    const GerberKitExportResult wrongTxt = exportFabLayer(
        doc, QStringLiteral("Top-Copper"), tmp.filePath("weird.txt"));
    REQUIRE(wrongTxt.ok);
    REQUIRE(wrongTxt.warnings.size() == 1);
    CHECK(wrongTxt.warnings.first().contains(QLatin1String("Excellon")));
    const GerberKitExportResult wrongGbr = exportFabLayer(
        doc, QStringLiteral("Drill"), tmp.filePath("drill_as.gbr"));
    REQUIRE(wrongGbr.ok);
    REQUIRE(wrongGbr.warnings.size() == 1);
    CHECK(wrongGbr.warnings.first().contains(QLatin1String("EXCELLON")));
    // Matching extensions stay silent.
    CHECK(exportFabLayer(doc, QStringLiteral("Top-Copper"),
                         tmp.filePath("ok.gtl"))
              .warnings.isEmpty());
    CHECK(exportFabLayer(doc, QStringLiteral("Drill"), tmp.filePath("ok.txt"))
              .warnings.isEmpty());

    // A hard mid-kit error must not leave a plausible partial kit behind:
    // the drill hole out of coordinate range fails AFTER brd.GTL was
    // written — the writer removes it and says so.
    {
        TransactionScope tx(doc, QStringLiteral("T"));
        auto far = std::make_unique<CircleEntity>(Vec2d{10524.0, 0.0}, 0.5);
        far->setLayerId(drill);
        doc.addEntity(std::move(far));
        tx.commit();
    }
    QTemporaryDir tmp2;
    REQUIRE(tmp2.isValid());
    const GerberKitExportResult fail =
        exportGerberKit(doc, tmp2.path(), QStringLiteral("brd"));
    REQUIRE(!fail.ok);
    CHECK(fail.error.contains(QLatin1String("exceeds")));
    CHECK(fail.error.contains(QLatin1String("removed")));
    CHECK(fail.files.empty());
    CHECK(!QFileInfo(tmp2.filePath(QStringLiteral("brd.GTL"))).exists());
    CHECK(!QFileInfo(tmp2.filePath(QStringLiteral("brd.TXT"))).exists());
    run(doc, QStringLiteral("UNDO")); // drop the out-of-range hole

    // PANELIZE typo guard: 2000 x 2000 x 2 fab entities = ~8 M clones,
    // over the 2 M cap — refused, document untouched.
    const size_t before = doc.entityCount();
    run(doc, QStringLiteral("PANELIZE 2000 2000 10 10"));
    CHECK(doc.entityCount() == before);
    // A sane grid still works.
    run(doc, QStringLiteral("PANELIZE 2 1 20 20"));
    CHECK(doc.entityCount() == before + 2);
}

// ---------------------------------------------------------------------------
// The 2D->GKO promise: a closed polyline on an Outline-role layer
// ---------------------------------------------------------------------------

TEST_CASE("Fab layer export: closed 2D outline becomes a clean .GKO",
          "[gerberkit][export]")
{
    Document doc;
    run(doc, QStringLiteral("PLINE 0,0 80,0 80,50 0,50 C"));
    run(doc, QStringLiteral("PLWIDTH 0.15 1"));
    run(doc, QStringLiteral("LAYER 0 ROLE Outline"));

    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString gko = tmp.filePath(QStringLiteral("board.GKO"));
    const GerberKitExportResult r =
        exportFabLayer(doc, QStringLiteral("0"), gko);
    INFO(r.error.toStdString());
    REQUIRE(r.ok);
    CHECK(r.files.front().entities == 1);
    CHECK(r.warnings.isEmpty()); // a real width -> no zero-aperture warning

    // The file re-parses as one closed 4-vertex contour at 0.15 mm.
    const GerberParseResult p = parseGerber(gko);
    REQUIRE(p.ok);
    Document doc2;
    const GerberImportResult ir =
        gerberToDocument(doc2, p.file, QStringLiteral("L"));
    REQUIRE(ir.ok);
    REQUIRE(doc2.entityCount() == 1);
    const auto* pl =
        dynamic_cast<const PolylineEntity*>(doc2.entity(doc2.drawOrder().front()));
    REQUIRE(pl != nullptr);
    CHECK(pl->width() == Approx(0.15).margin(1e-6));
    const BBox2d box = doc2.extents();
    CHECK(box.min.x == Approx(0.0 - 0.075).margin(1e-3));
    CHECK(box.max.x == Approx(80.0 + 0.075).margin(1e-3));
    CHECK(box.max.y == Approx(50.0 + 0.075).margin(1e-3));
}

// ---------------------------------------------------------------------------
// PANELIZE — synthetic: grid, fab-only, one undo
// ---------------------------------------------------------------------------

TEST_CASE("PANELIZE duplicates fab content only, one undo restores",
          "[panelize]")
{
    Document doc;
    const LayerId copper = doc.ensureLayer(QStringLiteral("Top-Copper"));
    doc.setLayerGerberRole(copper, QStringLiteral("Copper-Top"));
    const LayerId drill = doc.ensureLayer(QStringLiteral("Drill"));
    doc.setLayerGerberRole(drill, QStringLiteral("Drill"));
    const LayerId notes = doc.ensureLayer(QStringLiteral("Notes")); // no role
    {
        TransactionScope tx(doc, QStringLiteral("T"));
        auto pl = std::make_unique<PolylineEntity>(
            std::vector<PolyVertex>{{{0.0, 0.0}, 0.0}, {{5.0, 0.0}, 0.0}}, false);
        pl->setWidth(0.2);
        pl->setLayerId(copper);
        doc.addEntity(std::move(pl));
        auto c = std::make_unique<CircleEntity>(Vec2d{1.0, 1.0}, 0.4);
        c->setLayerId(drill);
        doc.addEntity(std::move(c));
        auto n = std::make_unique<LineEntity>(Vec2d{0.0, 0.0}, Vec2d{1.0, 1.0});
        n->setLayerId(notes);
        doc.addEntity(std::move(n));
        tx.commit();
    }
    REQUIRE(doc.entityCount() == 3);

    run(doc, QStringLiteral("PANELIZE 2 3 10 20"));
    // 2 fab entities x (6 - 1) new cells = 10 clones; the note is NOT fab.
    CHECK(doc.entityCount() == 13);

    // A clone landed at +10,+40 (col 1, row 2) with tags and width intact.
    int found = 0;
    for (const EntityId id : doc.drawOrder()) {
        const auto* pl = dynamic_cast<const PolylineEntity*>(doc.entity(id));
        if (!pl)
            continue;
        if (std::fabs(pl->vertices().front().pos.x - 10.0) < 1e-9 &&
            std::fabs(pl->vertices().front().pos.y - 40.0) < 1e-9) {
            ++found;
            CHECK(pl->width() == Approx(0.2));
        }
    }
    CHECK(found == 1);

    run(doc, QStringLiteral("UNDO"));
    CHECK(doc.entityCount() == 3); // ONE transaction

    // Degenerate grids refuse or no-op loudly.
    run(doc, QStringLiteral("PANELIZE 1 1 10 10"));
    CHECK(doc.entityCount() == 3);
}

// ---------------------------------------------------------------------------
// DXF bridge: constant width crosses both ways
// ---------------------------------------------------------------------------

#ifdef VIKICAD_HAS_DXF
TEST_CASE("DXF bridge: trace width round-trips as LWPOLYLINE constant width",
          "[dxf][gerberkit]")
{
    Document doc;
    doc.ensureLayer(QStringLiteral("Top-Copper"));
    const LayerId lid = doc.layerByName(QStringLiteral("Top-Copper"))->id;
    {
        TransactionScope tx(doc, QStringLiteral("T"));
        auto pl = std::make_unique<PolylineEntity>(
            std::vector<PolyVertex>{{{0.0, 0.0}, 0.0},
                                    {{12.5, 0.0}, 0.5},
                                    {{20.0, 5.0}, 0.0}},
            false);
        pl->setWidth(0.42);
        pl->setLayerId(lid);
        doc.addEntity(std::move(pl));
        tx.commit();
    }

    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());

    SECTION("2013 LWPOLYLINE code 43") {
        const QString dxf = tmp.filePath(QStringLiteral("w.dxf"));
        const DxfExportResult r = exportDxf(doc, dxf, QStringLiteral("2013"));
        REQUIRE(r.ok);
        const DxfImportResult ir = importDxf(dxf);
        REQUIRE(ir.ok);
        const auto* pl = dynamic_cast<const PolylineEntity*>(
            ir.document->entity(ir.document->drawOrder().front()));
        REQUIRE(pl != nullptr);
        CHECK(pl->width() == Approx(0.42).margin(1e-9));
        CHECK(pl->vertices()[1].bulge == Approx(0.5).margin(1e-9));
    }
    SECTION("R12 legacy POLYLINE codes 40/41") {
        const QString dxf = tmp.filePath(QStringLiteral("w12.dxf"));
        const DxfExportResult r = exportDxf(doc, dxf, QStringLiteral("R12"));
        REQUIRE(r.ok);
        const DxfImportResult ir = importDxf(dxf);
        REQUIRE(ir.ok);
        const auto* pl = dynamic_cast<const PolylineEntity*>(
            ir.document->entity(ir.document->drawOrder().front()));
        REQUIRE(pl != nullptr);
        CHECK(pl->width() == Approx(0.42).margin(1e-9));
    }
}
#endif

// ---------------------------------------------------------------------------
// THE end-to-end CAM scenario on the real kit A
// ---------------------------------------------------------------------------

TEST_CASE("CAM loop: edit kit A, export, re-import — every edit survives",
          "[gerberkit][export][kits]")
{
    if (!kitAPresent()) {
        SKIP("real Gerber kits not present on this machine");
    }
    Document doc;
    const GerberKitResult kit = importGerberKit(
        doc, QLatin1String(kKitRoot) + QLatin1String("/S5M0PCBA"));
    REQUIRE(kit.ok);

    // ---- pick the edit targets ------------------------------------------
    const Layer* topCopper = layerNamed(doc, "Top-Copper");
    const Layer* topSilk = layerNamed(doc, "Top-Silk");
    REQUIRE(topCopper);
    REQUIRE(topSilk);

    EntityId padId = kInvalidEntityId;
    Vec2d padPos;
    EntityId traceId = kInvalidEntityId;
    Vec2d traceStart, traceEnd;
    for (const EntityId id : doc.drawOrder()) {
        const Entity* e = doc.entity(id);
        if (!e || e->layerId() != topCopper->id)
            continue;
        if (padId == kInvalidEntityId) {
            if (const auto* ins = dynamic_cast<const InsertEntity*>(e)) {
                padId = id;
                padPos = ins->position;
            }
        }
        if (traceId == kInvalidEntityId) {
            if (const auto* pl = dynamic_cast<const PolylineEntity*>(e)) {
                if (pl->width() > 0.0) {
                    traceId = id;
                    traceStart = pl->vertices().front().pos;
                    traceEnd = pl->vertices().back().pos;
                }
            }
        }
    }
    REQUIRE(padId != kInvalidEntityId);
    REQUIRE(traceId != kInvalidEntityId);

    EntityId silkId = kInvalidEntityId;
    for (const EntityId id : doc.drawOrder()) {
        const Entity* e = doc.entity(id);
        if (e && e->layerId() == topSilk->id) {
            silkId = id;
            break;
        }
    }
    REQUIRE(silkId != kInvalidEntityId);
    const size_t silkBefore = entitiesOn(doc, "Top-Silk").size();
    const size_t drillBefore = entitiesOn(doc, "Drill").size();

    // ---- the four edits, all through the single CommandProcessor ---------
    run(doc, QStringLiteral("MOVE %1 0,0 2,0").arg(padId));      // pad +2 mm in X
    run(doc, QStringLiteral("PLWIDTH 0.42 %1").arg(traceId));    // trace width
    run(doc, QStringLiteral("ERASE %1").arg(silkId));            // silk stroke gone
    run(doc, QStringLiteral("LAYER Drill CURRENT"));             // new hole:
    run(doc, QStringLiteral("CIRCLE 45,-10 0.55"));              // d=1.1 mm

    CHECK(entitiesOn(doc, "Top-Silk").size() == silkBefore - 1);
    CHECK(entitiesOn(doc, "Drill").size() == drillBefore + 1);

    // ---- export the kit ---------------------------------------------------
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString dir1 = tmp.filePath(QStringLiteral("out1"));
    const GerberKitExportResult exp1 =
        exportGerberKit(doc, dir1, QStringLiteral("edited"));
    INFO(exp1.error.toStdString());
    REQUIRE(exp1.ok);

    // ---- re-import into a FRESH document ----------------------------------
    Document doc2;
    const GerberKitResult kit2 = importGerberKit(doc2, dir1);
    INFO(kit2.error.toStdString());
    REQUIRE(kit2.ok);

    // 1. the pad moved: on the COPPER layer one flash origin sits at
    // padPos+(2,0) and none is left at padPos (mask/paste flashes of other
    // layers legitimately still sit near the old spot).
    int atNew = 0, atOld = 0;
    for (const Entity* e : entitiesOn(doc2, "Top-Copper")) {
        if (const auto* ins = dynamic_cast<const InsertEntity*>(e)) {
            if ((ins->position - (padPos + Vec2d{2.0, 0.0})).length() < 1e-3)
                ++atNew;
            if ((ins->position - padPos).length() < 0.5)
                ++atOld;
        }
    }
    CHECK(atNew == 1);
    CHECK(atOld == 0);

    // 2. the trace width: the polyline with the SAME endpoints reads 0.42.
    int tracesAt042 = 0;
    for (const Entity* e : entitiesOn(doc2, "Top-Copper")) {
        const auto* pl = dynamic_cast<const PolylineEntity*>(e);
        if (!pl)
            continue;
        if ((pl->vertices().front().pos - traceStart).length() < 1e-3 &&
            (pl->vertices().back().pos - traceEnd).length() < 1e-3) {
            CHECK(pl->width() == Approx(0.42).margin(1e-6));
            ++tracesAt042;
        }
    }
    CHECK(tracesAt042 == 1);

    // 3. the erased silk stroke stayed erased (counts round-trip exactly).
    CHECK(entitiesOn(doc2, "Top-Silk").size() == silkBefore - 1);

    // 4. the new hole: d=1.1 plated at (45,-10) on the drill layer.
    int newHoles = 0;
    for (const Entity* e : entitiesOn(doc2, "Drill")) {
        const auto* c = dynamic_cast<const CircleEntity*>(e);
        if (!c)
            continue;
        if ((c->center() - Vec2d{45.0, -10.0}).length() < 1e-3) {
            CHECK(c->radius() == Approx(0.55).margin(1e-6));
            CHECK(c->extra().value(QLatin1String("plated")).toBool());
            ++newHoles;
        }
    }
    CHECK(newHoles == 1);
    CHECK(entitiesOn(doc2, "Drill").size() == drillBefore + 1);

    // ---- gerbv truth: the re-exported kit renders EXACTLY like the export.
    const QString gerbv = gerbvExe();
    if (gerbv.isEmpty())
        return; // semantic checks already passed; visual tier needs gerbv
    const QString dir2 = tmp.filePath(QStringLiteral("out2"));
    const GerberKitExportResult exp2 =
        exportGerberKit(doc2, dir2, QStringLiteral("edited"));
    REQUIRE(exp2.ok);
    for (const char* f : {"edited.GTL", "edited.GTO", "edited.TXT"}) {
        const QString a = QDir(dir1).filePath(QLatin1String(f));
        const QString b = QDir(dir2).filePath(QLatin1String(f));
        const QString pa = a + QStringLiteral(".png");
        const QString pb = b + QStringLiteral(".png");
        REQUIRE(renderWithGerbv(gerbv, a, pa));
        REQUIRE(renderWithGerbv(gerbv, b, pb));
        const InkMask ma = inkMask(pa);
        const InkMask mb = inkMask(pb);
        REQUIRE(!ma.img.isNull());
        REQUIRE(!mb.img.isNull());
        const int dist = dhashDistance(ma.img, mb.img);
        INFO(f << ": dhash=" << dist);
        CHECK(dist < 30);
        CHECK(std::fabs(ma.ink - mb.ink) * 100.0 <= 1.0);
    }
    // And the touched copper DIFFERS from the pristine original (the edits
    // really are in the file — same normalization, so identical files
    // would read 0).
    const QString orig = QLatin1String(kKitRoot) +
                         QLatin1String("/S5M0PCBA/S5M0PCBA1.GTL");
    const QString po = tmp.filePath(QStringLiteral("orig.GTL.png"));
    REQUIRE(renderWithGerbv(gerbv, orig, po));
    const InkMask mo = inkMask(po);
    const InkMask me =
        inkMask(QDir(dir1).filePath(QStringLiteral("edited.GTL")) +
                QStringLiteral(".png"));
    CHECK(dhashDistance(mo.img, me.img) > 0);
}

// ---------------------------------------------------------------------------
// PANELIZE on the real kit A: DRILLREPORT x4 + ~4x ink under gerbv
// ---------------------------------------------------------------------------

TEST_CASE("PANELIZE kit A 2x2: drills x4, ~4x ink under gerbv",
          "[panelize][kits]")
{
    if (!kitAPresent()) {
        SKIP("real Gerber kits not present on this machine");
    }
    Document doc;
    const GerberKitResult kit = importGerberKit(
        doc, QLatin1String(kKitRoot) + QLatin1String("/S5M0PCBA"));
    REQUIRE(kit.ok);
    const size_t before = doc.entityCount();
    const size_t drillsBefore =
        entitiesOn(doc, "Drill").size() + entitiesOn(doc, "Drill-NPTH").size();
    REQUIRE(drillsBefore == 182); // the .DRR total (locked by test_cam_inspect)

    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString singleGtl = tmp.filePath(QStringLiteral("single.GTL"));
    REQUIRE(exportFabLayer(doc, QStringLiteral("Top-Copper"), singleGtl).ok);

    // Board spans ~91 x 51 mm: 95 x 55 pitches keep the cells apart.
    run(doc, QStringLiteral("PANELIZE 2 2 95 55"));
    const size_t drillsAfter =
        entitiesOn(doc, "Drill").size() + entitiesOn(doc, "Drill-NPTH").size();
    CHECK(drillsAfter == 4 * 182); // DRILLREPORT counts live circles

    const QString panelGtl = tmp.filePath(QStringLiteral("panel.GTL"));
    REQUIRE(exportFabLayer(doc, QStringLiteral("Top-Copper"), panelGtl).ok);

    // One undo = the whole panel vanishes.
    run(doc, QStringLiteral("UNDO"));
    CHECK(doc.entityCount() == before);

    const QString gerbv = gerbvExe();
    if (gerbv.isEmpty())
        return; // counting tier passed; the ink tier needs gerbv
    const QString ps = singleGtl + QStringLiteral(".png");
    const QString pp = panelGtl + QStringLiteral(".png");
    REQUIRE(renderWithGerbv(gerbv, singleGtl, ps));
    REQUIRE(renderWithGerbv(gerbv, panelGtl, pp));
    const double ratio = double(rawInkCount(pp)) / double(rawInkCount(ps));
    INFO("panel/single ink ratio = " << ratio);
    CHECK(ratio > 4.0 * 0.95);
    CHECK(ratio < 4.0 * 1.05);
}

// ---------------------------------------------------------------------------
// DXF bridge on the real kit A: full-kit semantics survive the DXF hop
// ---------------------------------------------------------------------------

#ifdef VIKICAD_HAS_DXF
TEST_CASE("DXF bridge: kit A crosses DXF and keeps widths, pads and holes",
          "[dxf][gerberkit][kits]")
{
    if (!kitAPresent()) {
        SKIP("real Gerber kits not present on this machine");
    }
    Document doc;
    REQUIRE(importGerberKit(doc,
                            QLatin1String(kKitRoot) + QLatin1String("/S5M0PCBA"))
                .ok);

    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString dxf = tmp.filePath(QStringLiteral("kitA.dxf"));
    const DxfExportResult ex = exportDxf(doc, dxf, QStringLiteral("2013"));
    REQUIRE(ex.ok);
    CHECK(ex.skipped == 0);

    const DxfImportResult ir = importDxf(dxf);
    REQUIRE(ir.ok);
    const Document& doc2 = *ir.document;
    REQUIRE(doc2.entityCount() == doc.entityCount());

    // Sorted multiset comparisons with a tolerance (never round-to-grid: a
    // value ON a rounding boundary flips its bucket for a 1e-12 wobble):
    // trace widths, drill radii, pad flash positions.
    const auto sortedClose = [](const std::vector<double>& a,
                                const std::vector<double>& b, double tol) {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::fabs(a[i] - b[i]) > tol)
                return false;
        return true;
    };
    const auto widths = [](const Document& d) {
        std::vector<double> out;
        for (const EntityId id : d.drawOrder())
            if (const auto* pl =
                    dynamic_cast<const PolylineEntity*>(d.entity(id)))
                out.push_back(pl->width());
        std::sort(out.begin(), out.end());
        return out;
    };
    const auto radii = [](const Document& d) {
        std::vector<double> out;
        for (const EntityId id : d.drawOrder())
            if (const auto* c = dynamic_cast<const CircleEntity*>(d.entity(id)))
                out.push_back(c->radius());
        std::sort(out.begin(), out.end());
        return out;
    };
    const auto flashes = [](const Document& d) {
        std::vector<double> out; // x and y interleaved after a stable sort
        std::vector<std::pair<double, double>> pos;
        for (const EntityId id : d.drawOrder())
            if (const auto* i = dynamic_cast<const InsertEntity*>(d.entity(id)))
                pos.push_back({i->position.x, i->position.y});
        std::sort(pos.begin(), pos.end());
        for (const auto& p : pos) {
            out.push_back(p.first);
            out.push_back(p.second);
        }
        return out;
    };
    CHECK(sortedClose(widths(doc), widths(doc2), 1e-6));
    CHECK(sortedClose(radii(doc), radii(doc2), 1e-6));
    CHECK(sortedClose(flashes(doc), flashes(doc2), 1e-6));
}
#endif
