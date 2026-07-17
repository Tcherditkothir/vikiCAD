// "Open a Gerber kit" — directory recognition, content sniffing, layer
// naming/colors/paint order, drill split, outline election, X2 override,
// and the one-transaction contract.
//
// Two tiers, like test_gerber.cpp:
//  - synthetic kits written into a QTemporaryDir (self-contained);
//  - the real Altium Designer 18 kits under /home/lex/computer/pcb-ref/
//    (private, machine-local): those tests SKIP when the directory is absent.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "cmd/CommandProcessor.h"
#include "doc/Annotations.h"
#include "doc/Block.h"
#include "doc/Document.h"
#include "doc/Entities.h"
#include "io/GerberKit.h"
#include "snap/SnapEngine.h"

using namespace viki;
using Catch::Approx;

namespace {

const char* kKitRoot = "/home/lex/computer/pcb-ref";

bool kitsPresent()
{
    return QDir(QLatin1String(kKitRoot) + QLatin1String("/S5M0PCBA")).exists() &&
           QDir(QLatin1String(kKitRoot) + QLatin1String("/S5M0PCBB")).exists();
}

void writeFile(const QString& dir, const QString& name, const QByteArray& data)
{
    QFile f(dir + QLatin1Char('/') + name);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(data);
}

// Minimal RS-274X body (mm, 2.4): one 10 mm dark draw with a 0.2 mm round
// aperture. `extraHeader` is inserted before %FS (X2 attributes...).
QByteArray simpleGerber(const QByteArray& extraHeader = {},
                        const QByteArray& extraBody = {})
{
    return "G04 synthetic kit layer*\n" + extraHeader +
           "%FSLAX24Y24*%\n"
           "%MOMM*%\n"
           "G01*\n"
           "%ADD10C,0.2*%\n"
           "D10*\n"
           "X0Y0D02*\n"
           "X100000Y0D01*\n" +
           extraBody + "M02*\n";
}

// Header-only Gerber (parses fine, zero graphical objects) — Altium's usual
// empty GKO.
QByteArray emptyGerber()
{
    return "G04 empty layer*\n"
           "%FSLAX24Y24*%\n"
           "%MOMM*%\n"
           "M02*\n";
}

// Thin closed 10 x 5 mm rectangle: a PLAUSIBLE board contour (strokes that
// cover the kit extent). simpleGerber's first draw is the bottom edge.
QByteArray contourGerber()
{
    return simpleGerber({},
                        "X100000Y50000D01*\n"
                        "X0Y50000D01*\n"
                        "X0Y0D01*\n");
}

// Small FILLED G36 rectangle (2 x 1 mm), zero strokes: what a real GKO
// keepout zone looks like (S5M0PCBB) — must never be elected Outline.
QByteArray keepoutGerber()
{
    return "G04 keepout zone*\n"
           "%FSLAX24Y24*%\n"
           "%MOMM*%\n"
           "G01*\n"
           "G36*\n"
           "X0Y0D02*\n"
           "X20000Y0D01*\n"
           "X20000Y10000D01*\n"
           "X0Y10000D01*\n"
           "X0Y0D01*\n"
           "G37*\n"
           "M02*\n";
}

// Path of the file that landed on `layerName`, empty if none.
QString sourceOfLayer(const GerberKitResult& r, const char* layerName)
{
    for (const GerberKitFile& f : r.files)
        if (f.layerName == QLatin1String(layerName))
            return f.path;
    return {};
}

const Layer* layerNamed(Document& doc, const char* name)
{
    return doc.layerByName(QLatin1String(name));
}

int entitiesOnLayer(const Document& doc, const Layer* layer)
{
    REQUIRE(layer != nullptr);
    int n = 0;
    for (const EntityId id : doc.drawOrder())
        if (const Entity* e = doc.entity(id); e && e->layerId() == layer->id)
            ++n;
    return n;
}

} // namespace

// ---------------------------------------------------------------------------
// Synthetic kits
// ---------------------------------------------------------------------------

TEST_CASE("GerberKit: recognition, sniffing, colors, order, drill split, one undo",
          "[gerberkit]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString dir = tmp.path();

    // Copper with a clear-polarity (LPC) second draw.
    writeFile(dir, QStringLiteral("board.GTL"),
              simpleGerber({}, "%LPC*%\nX0Y50000D02*\nX100000Y50000D01*\n"));
    writeFile(dir, QStringLiteral("board.GBL"), simpleGerber());
    writeFile(dir, QStringLiteral("board.GTO"), simpleGerber());
    // Altium-style empty keepout: the outline election must fall to GM1
    // (whose strokes cover the kit extent — a plausible contour).
    writeFile(dir, QStringLiteral("board.GKO"), emptyGerber());
    writeFile(dir, QStringLiteral("board.GM1"), contourGerber());
    writeFile(dir, QStringLiteral("board.GM13"), simpleGerber());
    // Drill: T1 plated (1 mm), T2 non-plated (3 mm). INCH,TZ 2:4.
    writeFile(dir, QStringLiteral("board.TXT"),
              "M48\n"
              ";FILE_FORMAT=2:4\n"
              "INCH,TZ\n"
              ";TYPE=PLATED\n"
              "T1C0.03937\n"
              ";TYPE=NON_PLATED\n"
              "T2C0.11811\n"
              "%\n"
              "T1\n"
              "X10000Y10000\n"
              "T2\n"
              "X20000Y20000\n"
              "M30\n");
    // Decoys: report files and a TXT that is NOT a drill file.
    writeFile(dir, QStringLiteral("Status Report.Txt"),
              "Output: NC Drill Files\nType  : NC Drill\n");
    writeFile(dir, QStringLiteral("board.DRR"), "Tool Table\nT1 holes\n");
    writeFile(dir, QStringLiteral("board.REP"), "Aperture report\n");

    Document doc;
    const GerberKitResult r = importGerberKit(doc, dir);
    INFO(r.error.toStdString());
    REQUIRE(r.ok);

    // Layers in paint order: copper at the bottom, outline + drills on top.
    const QStringList expected{
        QStringLiteral("Bottom-Copper"), QStringLiteral("Top-Copper"),
        QStringLiteral("Top-Silk"),      QStringLiteral("Mech-13"),
        QStringLiteral("Outline"),       QStringLiteral("Drill"),
        QStringLiteral("Drill-NPTH")};
    CHECK(r.layers == expected);

    // The empty GKO never created a layer; GM1 was promoted to Outline.
    CHECK(layerNamed(doc, "Keepout") == nullptr);
    CHECK(layerNamed(doc, "Mech-1") == nullptr);
    REQUIRE(layerNamed(doc, "Outline") != nullptr);

    // Decoys skipped: content sniff for the fake TXT, extension for reports.
    REQUIRE(r.skipped.size() == 4); // GKO + Status Report.Txt + DRR + REP
    CHECK(r.skipped.filter(QStringLiteral("Status Report.Txt")).size() == 1);
    CHECK(r.skipped.filter(QStringLiteral("board.DRR")).size() == 1);
    CHECK(r.skipped.filter(QStringLiteral("board.REP")).size() == 1);
    CHECK(r.skipped.filter(QStringLiteral("board.GKO")).size() == 1);

    // Default colors (readable set from the mission brief).
    CHECK(layerNamed(doc, "Top-Copper")->rgb == 0xE53935);
    CHECK(layerNamed(doc, "Bottom-Copper")->rgb == 0x3D7EFF);
    CHECK(layerNamed(doc, "Top-Silk")->rgb == 0xF2D544);
    CHECK(layerNamed(doc, "Outline")->rgb == 0xFF00FF);
    CHECK(layerNamed(doc, "Drill")->rgb == 0x000000u);
    CHECK(layerNamed(doc, "Drill-NPTH")->rgb == 0x3C3C3C);

    // G2: the paint order is EXPLICIT on the layers (rank, lower first) and
    // the CAM role rides along as reassignable metadata.
    CHECK(layerNamed(doc, "Bottom-Copper")->rank <
          layerNamed(doc, "Top-Copper")->rank);
    CHECK(layerNamed(doc, "Top-Copper")->rank <
          layerNamed(doc, "Outline")->rank);
    CHECK(layerNamed(doc, "Outline")->rank < layerNamed(doc, "Drill")->rank);
    CHECK(layerNamed(doc, "Drill")->rank < layerNamed(doc, "Drill-NPTH")->rank);
    CHECK(layerNamed(doc, "Top-Copper")->gerberRole ==
          QStringLiteral("Copper-Top"));
    CHECK(layerNamed(doc, "Bottom-Copper")->gerberRole ==
          QStringLiteral("Copper-Bottom"));
    CHECK(layerNamed(doc, "Top-Silk")->gerberRole == QStringLiteral("Silk"));
    CHECK(layerNamed(doc, "Outline")->gerberRole == QStringLiteral("Outline"));
    CHECK(layerNamed(doc, "Drill")->gerberRole == QStringLiteral("Drill"));
    CHECK(layerNamed(doc, "Drill-NPTH")->gerberRole == QStringLiteral("Drill"));
    // Every imported layer keeps the default opacity: alpha != 100 would
    // change the reference renders (gerber-ref-diff guards this too).
    for (const Layer& l : doc.layers())
        CHECK(l.alpha == 100);

    // Entities landed on their layers; paint order = draw order: the very
    // first entity belongs to the bottom copper, the very last to a drill.
    CHECK(entitiesOnLayer(doc, layerNamed(doc, "Top-Copper")) == 2); // dark + LPC
    CHECK(entitiesOnLayer(doc, layerNamed(doc, "Drill")) == 1);
    CHECK(entitiesOnLayer(doc, layerNamed(doc, "Drill-NPTH")) == 1);
    REQUIRE(doc.entityCount() == size_t(r.entities));
    const Entity* first = doc.entity(doc.drawOrder().front());
    const Entity* last = doc.entity(doc.drawOrder().back());
    CHECK(first->layerId() == layerNamed(doc, "Bottom-Copper")->id);
    CHECK(last->layerId() == layerNamed(doc, "Drill-NPTH")->id);

    // LPC entity kept its clear-polarity marker.
    bool sawClear = false;
    for (const EntityId id : doc.drawOrder())
        if (const Entity* e = doc.entity(id);
            e && e->extra().value(QLatin1String("gpol")).toString() ==
                     QLatin1String("C"))
            sawClear = true;
    CHECK(sawClear);

    // Drill geometry: plated hit at 1 in = 25.4 mm, radius 0.5 mm.
    {
        const Layer* drill = layerNamed(doc, "Drill");
        const CircleEntity* c = nullptr;
        for (const EntityId id : doc.drawOrder())
            if (const auto* e = dynamic_cast<const CircleEntity*>(doc.entity(id));
                e && e->layerId() == drill->id)
                c = e;
        REQUIRE(c != nullptr);
        CHECK(std::abs(c->center().x - 25.4) < 1e-6);
        CHECK(std::abs(c->radius() - 0.5) < 1e-3);
        CHECK(c->extra().value(QLatin1String("plated")).toBool());
    }

    // ONE transaction: a single undo restores the empty document.
    CHECK(doc.undo() == QStringLiteral("GERBERKIT"));
    CHECK(doc.entityCount() == 0);
    CHECK(doc.redo() == QStringLiteral("GERBERKIT"));
    CHECK(doc.entityCount() == size_t(r.entities));
}

TEST_CASE("GerberKit: X2 TF.FileFunction prevails over the extension", "[gerberkit]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString dir = tmp.path();

    // Extension says nothing ('.pho'), X2 says top soldermask — both the
    // naked %TF form and Altium's comment form must work (parser handles
    // both; use one of each here).
    writeFile(dir, QStringLiteral("weird.pho"),
              simpleGerber("%TF.FileFunction,Soldermask,Top*%\n"));
    // Extension says mechanical 7, X2 says board profile -> Outline wins,
    // and the GKO candidate below loses the election to the explicit profile.
    writeFile(dir, QStringLiteral("board.GM7"),
              simpleGerber("G04 #@! TF.FileFunction,Profile,NP*\n"));
    writeFile(dir, QStringLiteral("board.GKO"), simpleGerber());

    Document doc;
    const GerberKitResult r = importGerberKit(doc, dir);
    INFO(r.error.toStdString());
    REQUIRE(r.ok);

    CHECK(layerNamed(doc, "Top-Mask") != nullptr);
    CHECK(layerNamed(doc, "Outline") != nullptr);
    CHECK(layerNamed(doc, "Mech-7") == nullptr);
    // The non-empty GKO keeps its keepout fallback (profile already taken).
    // Keepout zones are painted FIRST (below copper): a filled keepout must
    // never mask the board.
    CHECK(layerNamed(doc, "Keepout") != nullptr);
    CHECK(r.layers == QStringList{QStringLiteral("Keepout"),
                                  QStringLiteral("Top-Mask"),
                                  QStringLiteral("Outline")});
}

TEST_CASE("GerberKit: outline election rejects implausible candidates",
          "[gerberkit]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString dir = tmp.path();
    // Copper that defines the kit extent (10 x 5 mm).
    writeFile(dir, QStringLiteral("board.GTL"), contourGerber());

    SECTION("a filled keepout GKO loses to a contour-shaped GM1 (S5M0PCBB)")
    {
        writeFile(dir, QStringLiteral("board.GKO"), keepoutGerber());
        writeFile(dir, QStringLiteral("board.GM1"), contourGerber());
        Document doc;
        const GerberKitResult r = importGerberKit(doc, dir);
        REQUIRE(r.ok);
        CHECK(sourceOfLayer(r, "Outline").endsWith(QStringLiteral(".GM1")));
        CHECK(layerNamed(doc, "Keepout") != nullptr);
        CHECK(layerNamed(doc, "Mech-1") == nullptr);
        // The keepout zone is painted below the copper, never on top of it.
        CHECK(r.layers.indexOf(QStringLiteral("Keepout")) <
              r.layers.indexOf(QStringLiteral("Top-Copper")));
    }
    SECTION("a contour-shaped GKO still wins over GM1 (priority preserved)")
    {
        writeFile(dir, QStringLiteral("board.GKO"), contourGerber());
        writeFile(dir, QStringLiteral("board.GM1"), contourGerber());
        Document doc;
        const GerberKitResult r = importGerberKit(doc, dir);
        REQUIRE(r.ok);
        CHECK(sourceOfLayer(r, "Outline").endsWith(QStringLiteral(".GKO")));
        CHECK(layerNamed(doc, "Mech-1") != nullptr);
        CHECK(layerNamed(doc, "Keepout") == nullptr);
    }
    SECTION("GM13 is a candidate when GKO/GM1 are absent (S5M0PCBA)")
    {
        writeFile(dir, QStringLiteral("board.GM13"), contourGerber());
        Document doc;
        const GerberKitResult r = importGerberKit(doc, dir);
        REQUIRE(r.ok);
        CHECK(sourceOfLayer(r, "Outline").endsWith(QStringLiteral(".GM13")));
        CHECK(layerNamed(doc, "Mech-13") == nullptr);
    }
    SECTION("no plausible candidate -> no Outline layer at all")
    {
        writeFile(dir, QStringLiteral("board.GKO"), keepoutGerber());
        Document doc;
        const GerberKitResult r = importGerberKit(doc, dir);
        REQUIRE(r.ok);
        CHECK(layerNamed(doc, "Outline") == nullptr);
        CHECK(layerNamed(doc, "Keepout") != nullptr);
    }
}

TEST_CASE("GerberKit: a broken file is skipped in a directory, fatal alone",
          "[gerberkit]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString dir = tmp.path();
    writeFile(dir, QStringLiteral("good.GTL"), simpleGerber());
    // Sniffs as Gerber (G04 comment) but fails to parse: D01 before any
    // interpolation mode.
    writeFile(dir, QStringLiteral("bad.GBL"),
              "G04 truncated by a bad transfer*\n"
              "%FSLAX24Y24*%\n%MOMM*%\nD01*\nM02*\n");

    SECTION("directory import: the broken layer is skipped with a warning")
    {
        Document doc;
        const GerberKitResult r = importGerberKit(doc, dir);
        INFO(r.error.toStdString());
        REQUIRE(r.ok);
        CHECK(r.layers == QStringList{QStringLiteral("Top-Copper")});
        CHECK(r.skipped.filter(QStringLiteral("bad.GBL")).size() == 1);
        CHECK(r.warnings.filter(QStringLiteral("bad.GBL")).size() == 1);
        CHECK(doc.entityCount() == 1);
    }
    SECTION("explicit single-file import of the same file stays a hard error")
    {
        Document doc;
        const GerberKitResult r =
            importGerberKit(doc, dir + QStringLiteral("/bad.GBL"));
        CHECK_FALSE(r.ok);
        CHECK(doc.entityCount() == 0);
    }
}

TEST_CASE("GerberKit: single-file import and error paths", "[gerberkit]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString dir = tmp.path();

    writeFile(dir, QStringLiteral("only.GTL"), simpleGerber());
    writeFile(dir, QStringLiteral("junk.bin"), "just some text, no fab data");

    SECTION("a single Gerber file imports onto its role layer")
    {
        Document doc;
        const GerberKitResult r =
            importGerberKit(doc, dir + QStringLiteral("/only.GTL"));
        INFO(r.error.toStdString());
        REQUIRE(r.ok);
        CHECK(r.layers == QStringList{QStringLiteral("Top-Copper")});
        CHECK(doc.entityCount() == 1);
    }
    SECTION("the content sniff helper drives the GUI single-file open path")
    {
        CHECK(looksLikeGerberOrExcellon(dir + QStringLiteral("/only.GTL")));
        CHECK_FALSE(looksLikeGerberOrExcellon(dir + QStringLiteral("/junk.bin")));
        CHECK_FALSE(looksLikeGerberOrExcellon(dir + QStringLiteral("/nope")));
        CHECK_FALSE(looksLikeGerberOrExcellon(dir)); // directories: kit path
    }
    SECTION("a non-fab file is a clean error, not a crash")
    {
        Document doc;
        const GerberKitResult r =
            importGerberKit(doc, dir + QStringLiteral("/junk.bin"));
        CHECK_FALSE(r.ok);
        CHECK(doc.entityCount() == 0);
    }
    SECTION("a missing path is a clean error")
    {
        Document doc;
        const GerberKitResult r =
            importGerberKit(doc, dir + QStringLiteral("/nope"));
        CHECK_FALSE(r.ok);
    }
}

TEST_CASE("GerberKit: a VALID but empty fab file opens (empty doc + warning)",
          "[gerberkit]")
{
    // Altium ships header-only layers (PCBA's GKO and GM1 are just a header
    // + M02). gerbv opens them as an empty image; so must we — a lone empty
    // file is an EMPTY DOCUMENT with a warning, never a failed open.
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString dir = tmp.path();
    writeFile(dir, QStringLiteral("empty.GKO"), emptyGerber());

    SECTION("lone empty file: ok, zero entities, warning tells why")
    {
        Document doc;
        const GerberKitResult r =
            importGerberKit(doc, dir + QStringLiteral("/empty.GKO"));
        INFO(r.error.toStdString());
        REQUIRE(r.ok);
        CHECK(doc.entityCount() == 0);
        CHECK(r.layers.isEmpty());
        REQUIRE_FALSE(r.warnings.isEmpty());
        CHECK(r.warnings.first().contains(QStringLiteral("empty")));
    }
    SECTION("directory of ONLY empty fab files: same contract")
    {
        Document doc;
        const GerberKitResult r = importGerberKit(doc, dir);
        INFO(r.error.toStdString());
        REQUIRE(r.ok);
        CHECK(doc.entityCount() == 0);
        REQUIRE_FALSE(r.warnings.isEmpty());
        CHECK(r.warnings.first().contains(QStringLiteral("empty")));
    }
    SECTION("directory mixing empty and real layers: unchanged, empty skipped")
    {
        writeFile(dir, QStringLiteral("board.GTL"), simpleGerber());
        Document doc;
        const GerberKitResult r = importGerberKit(doc, dir);
        INFO(r.error.toStdString());
        REQUIRE(r.ok);
        CHECK(r.layers == QStringList{QStringLiteral("Top-Copper")});
        CHECK(doc.entityCount() == 1);
        CHECK(!r.skipped.filter(QStringLiteral("empty.GKO")).isEmpty());
    }
}

// ---------------------------------------------------------------------------
// Real Altium kits (skip when /home/lex/computer/pcb-ref is absent)
// ---------------------------------------------------------------------------

TEST_CASE("GerberKit: real S5M0PCBA kit end to end", "[gerberkit][kits]")
{
    if (!kitsPresent())
        SKIP("real Gerber kits not present on this machine");

    Document doc;
    const GerberKitResult r =
        importGerberKit(doc, QLatin1String(kKitRoot) + QLatin1String("/S5M0PCBA"));
    INFO(r.error.toStdString());
    REQUIRE(r.ok);

    // The full stack, one layer per non-empty file. PCBA's GKO and GM1 are
    // both header-only (verified by inspection); the real board contour is
    // drawn on GM13 (thin strokes covering the whole board), so the election
    // must promote GM13 to Outline — the board is never left contour-less.
    for (const char* name :
         {"Top-Copper", "Bottom-Copper", "Top-Mask", "Bottom-Mask", "Top-Silk",
          "Bottom-Silk", "Top-Paste", "Bottom-Paste", "Top-Pads", "Bottom-Pads",
          "Outline", "Mech-15", "Drill", "Drill-NPTH"}) {
        INFO(name);
        CHECK(layerNamed(doc, name) != nullptr);
    }
    CHECK(sourceOfLayer(r, "Outline").endsWith(QStringLiteral(".GM13")));
    CHECK(layerNamed(doc, "Mech-13") == nullptr);
    CHECK(r.skipped.filter(QStringLiteral(".GKO")).size() == 1);
    CHECK(r.skipped.filter(QStringLiteral(".GM1")).size() == 1);
    CHECK(r.skipped.filter(QStringLiteral("Status Report.Txt")).size() == 1);

    // Drill split against the .TXT ground truth: 180 plated + 2 non-plated.
    CHECK(entitiesOnLayer(doc, layerNamed(doc, "Drill")) == 180);
    CHECK(entitiesOnLayer(doc, layerNamed(doc, "Drill-NPTH")) == 2);

    // A dense real board: thousands of entities, all inside one transaction.
    CHECK(doc.entityCount() > 1000);
    CHECK(doc.undo() == QStringLiteral("GERBERKIT"));
    CHECK(doc.entityCount() == 0);
}

TEST_CASE("GerberKit: real S5M0PCBB kit — GM1 carries the outline, GKO is a keepout",
          "[gerberkit][kits]")
{
    if (!kitsPresent())
        SKIP("real Gerber kits not present on this machine");

    Document doc;
    const GerberKitResult r =
        importGerberKit(doc, QLatin1String(kKitRoot) + QLatin1String("/S5M0PCBB"));
    INFO(r.error.toStdString());
    REQUIRE(r.ok);

    // PCBB's GKO is a single FILLED G36 rectangle over the ESP32 antenna
    // zone (~17.7 x 6.5 mm, verified by inspection) — a keepout, NOT the
    // board contour. GM1 carries the actual contour geometry: the shaped
    // top edge of the board (notch + connector tabs, thin 0.1 mm strokes
    // spanning 68 % of the board width). The election must reject the
    // keepout (zero strokes, small on both axes) and promote GM1.
    REQUIRE(layerNamed(doc, "Outline") != nullptr);
    CHECK(sourceOfLayer(r, "Outline").endsWith(QStringLiteral(".GM1")));
    CHECK(layerNamed(doc, "Mech-1") == nullptr);
    CHECK(entitiesOnLayer(doc, layerNamed(doc, "Outline")) >= 1);

    // The GKO keepout keeps its role and paints BELOW the copper: the filled
    // magenta slab must never mask the antenna area again.
    REQUIRE(layerNamed(doc, "Keepout") != nullptr);
    CHECK(sourceOfLayer(r, "Keepout").endsWith(QStringLiteral(".GKO")));
    CHECK(entitiesOnLayer(doc, layerNamed(doc, "Keepout")) == 1);
    CHECK(r.layers.indexOf(QStringLiteral("Keepout")) <
          r.layers.indexOf(QStringLiteral("Bottom-Copper")));

    for (const char* name : {"Mech-4", "Mech-13", "Mech-15", "Mech-16"}) {
        INFO(name);
        CHECK(layerNamed(doc, name) != nullptr);
    }
    CHECK(doc.entityCount() > 1000);
}

TEST_CASE("GerberKit: measuring on the real kit — pad snaps, MINDIST, DIM",
          "[gerberkit][kits][mindist]")
{
    if (!kitsPresent())
        SKIP("real Gerber kits not present on this machine");

    Document doc;
    SelectionSet sel;
    CommandContext ctx{doc, sel};
    CommandProcessor processor{ctx};
    registerBuiltinCommands(processor);

    const GerberKitResult r =
        importGerberKit(doc, QLatin1String(kKitRoot) + QLatin1String("/S5M0PCBA"));
    REQUIRE(r.ok);

    // Two flashed pads (inserts) at distinct positions.
    std::vector<const InsertEntity*> pads;
    for (const EntityId id : doc.drawOrder())
        if (const auto* ins = dynamic_cast<const InsertEntity*>(doc.entity(id)))
            pads.push_back(ins);
    REQUIRE(pads.size() >= 2);
    const InsertEntity* p0 = pads.front();
    const InsertEntity* p1 = nullptr;
    for (const InsertEntity* p : pads)
        if (p != p0 && p->position.distanceTo(p0->position) > 1.0) {
            p1 = p;
            break;
        }
    REQUIRE(p1 != nullptr);

    // 1. The pad center is reachable with the CENTER osnap alone (cursor
    //    slightly off, endpoint snapping irrelevant) — pad-to-pad dimensioning.
    SnapSettings snaps;
    snaps.endpoint = snaps.node = snaps.midpoint = snaps.quadrant = false;
    snaps.intersection = snaps.perpendicular = snaps.tangent = snaps.nearest = false;
    snaps.center = true;
    const auto snap =
        snapQuery(doc, p0->position + Vec2d{0.05, 0.05}, 0.3, snaps, std::nullopt);
    REQUIRE(snap);
    CHECK(snap->kind == SnapKind::Center);
    CHECK(snap->point.x == Approx(p0->position.x).margin(1e-9));
    CHECK(snap->point.y == Approx(p0->position.y).margin(1e-9));

    // 2. MINDIST between two real drill hits, checked against the by-hand
    //    formula |c1-c2| - r1 - r2 from the drills' own geometry.
    const Layer* drill = layerNamed(doc, "Drill");
    REQUIRE(drill != nullptr);
    std::vector<const CircleEntity*> drills;
    for (const EntityId id : doc.drawOrder())
        if (const auto* c = dynamic_cast<const CircleEntity*>(doc.entity(id));
            c && c->layerId() == drill->id)
            drills.push_back(c);
    REQUIRE(drills.size() >= 2);
    const CircleEntity* d0 = drills[0];
    const CircleEntity* d1 = drills[1];
    const double expected =
        d0->center().distanceTo(d1->center()) - d0->radius() - d1->radius();

    ctx.clearMessages();
    REQUIRE(processor
                .submit(QStringLiteral("MINDIST %1 %2").arg(d0->id()).arg(d1->id()),
                        /*strict=*/true)
                .ok);
    QJsonObject json;
    for (const QString& m : ctx.messages()) {
        const auto parsed = QJsonDocument::fromJson(m.toUtf8());
        if (parsed.isObject() && parsed.object().contains(QStringLiteral("mindist")))
            json = parsed.object()[QStringLiteral("mindist")].toObject();
    }
    REQUIRE_FALSE(json.isEmpty());
    CHECK(json[QStringLiteral("mm")].toDouble() == Approx(expected).epsilon(1e-9));
    CHECK(json[QStringLiteral("method")].toString() == QStringLiteral("exact"));

    // 3. An aligned dimension from pad center to pad center (the points the
    //    Center osnap would deliver) lands as a dimension entity carrying
    //    exactly those definition points.
    const auto pt = [](const Vec2d& p) {
        return QStringLiteral("%1,%2").arg(p.x, 0, 'f', 6).arg(p.y, 0, 'f', 6);
    };
    const Vec2d mid = (p0->position + p1->position) * 0.5 + Vec2d{5, 5};
    const auto before = doc.entityCount();
    REQUIRE(processor
                .submit(QStringLiteral("DIMALIGNED %1 %2 %3")
                            .arg(pt(p0->position), pt(p1->position), pt(mid)),
                        /*strict=*/true)
                .ok);
    REQUIRE(doc.entityCount() == before + 1);
    const auto* dim =
        dynamic_cast<const DimensionEntity*>(doc.entity(doc.drawOrder().back()));
    REQUIRE(dim != nullptr);
    CHECK(dim->kind == DimensionEntity::Kind::Aligned);
    CHECK(dim->a.x == Approx(p0->position.x).margin(1e-5));
    CHECK(dim->a.y == Approx(p0->position.y).margin(1e-5));
    CHECK(dim->b.x == Approx(p1->position.x).margin(1e-5));
    CHECK(dim->b.y == Approx(p1->position.y).margin(1e-5));
}
