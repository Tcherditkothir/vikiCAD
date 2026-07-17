// Excellon (NC drill) parser + entity conversion (G1 stage).
//
// Two tiers:
//  - synthetic goldens committed in tests/golden/excellon/, one dialect trait
//    each, with expected values hand-decoded in comments;
//  - the real Altium Designer 18 kits under /home/lex/computer/pcb-ref/
//    (private, machine-local): those tests SKIP when the directory is absent.
//    Ground truth there is the .DRR report (hole count / size / plating per
//    tool), parsed independently of our Excellon parser.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>

#include <map>

#include "doc/Document.h"
#include "doc/Entities.h"
#include "io/ExcellonIo.h"
#include "render/Primitives.h"

using namespace viki;
using Catch::Approx;

namespace {

QString goldenPath(const char* name)
{
    return QStringLiteral(VIKICAD_GOLDEN_DIR "/excellon/") + QLatin1String(name);
}

const char* kKitRoot = "/home/lex/computer/pcb-ref";

bool kitsPresent()
{
    return QDir(QLatin1String(kKitRoot) + QLatin1String("/S5M0PCBA")).exists() &&
           QDir(QLatin1String(kKitRoot) + QLatin1String("/S5M0PCBB")).exists();
}

ExcellonFile parseGolden(const char* name)
{
    const ExcellonParseResult r = parseExcellon(goldenPath(name));
    INFO(r.error.toStdString());
    REQUIRE(r.ok);
    return r.file;
}

// Circles of the document in draw order.
std::vector<const CircleEntity*> circlesOf(const Document& doc)
{
    std::vector<const CircleEntity*> out;
    for (const EntityId id : doc.drawOrder())
        if (const auto* c = dynamic_cast<const CircleEntity*>(doc.entity(id)))
            out.push_back(c);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Synthetic goldens
// ---------------------------------------------------------------------------

TEST_CASE("Excellon: Altium dialect — FILE_FORMAT comment, INCH,TZ, sections, "
          "modal and negative coordinates", "[excellon]")
{
    // tz_inch.txt mirrors the real Altium 18 header. Hand-decode (2:5 forced
    // by ;FILE_FORMAT, INCH,TZ = trailing zeros KEPT = leading suppressed,
    // so value = integer / 1e5 inches):
    //   X94488  ->  0.94488 in = 23.999952 mm   (the canonical arithmetic check)
    //   Y-35827 -> -0.35827 in = -9.100058 mm
    //   X100000 ->  1.00000 in = 25.4 mm        (Y stays modal at -9.100058)
    //   Y50000  ->  0.50000 in = 12.7 mm        (X stays modal at 25.4)
    //   T1 C0.01181 -> 0.299974 mm    T2 C0.03937 -> 0.999998 mm
    //   T3 C0.09449 -> 2.400046 mm, NON_PLATED section
    const ExcellonFile f = parseGolden("tz_inch.txt");

    CHECK(f.unit == ExcellonUnit::Inches);
    CHECK(f.format.suppressLeading);
    CHECK(f.format.suppressionKnown);
    CHECK(f.format.fromComment);
    CHECK(f.format.intDigits == 2);
    CHECK(f.format.decDigits == 5);
    CHECK(f.sawEnd);
    INFO(f.warnings.join(QStringLiteral(" | ")).toStdString());
    CHECK(f.warnings.isEmpty());

    REQUIRE(f.tools.size() == 3);
    CHECK(f.tools.at(1).diameter == Approx(0.299974).margin(1e-9));
    CHECK(f.tools.at(1).plated);
    CHECK(f.tools.at(2).diameter == Approx(0.999998).margin(1e-9));
    CHECK(f.tools.at(2).plated);
    CHECK(f.tools.at(3).diameter == Approx(2.400046).margin(1e-9));
    CHECK_FALSE(f.tools.at(3).plated);

    REQUIRE(f.hits.size() == 5);
    CHECK(f.hits[0].tool == 1);
    CHECK(f.hits[0].pos.x == Approx(23.999952).margin(1e-9));
    CHECK(f.hits[0].pos.y == Approx(-9.100058).margin(1e-9));
    CHECK(f.hits[1].pos.x == Approx(25.4).margin(1e-9));
    CHECK(f.hits[1].pos.y == Approx(-9.100058).margin(1e-9)); // Y modal
    CHECK(f.hits[2].pos.x == Approx(25.4).margin(1e-9));      // X modal
    CHECK(f.hits[2].pos.y == Approx(12.7).margin(1e-9));
    CHECK(f.hits[3].tool == 2);
    CHECK(f.hits[3].pos.x == Approx(0.0).margin(1e-12));
    CHECK(f.hits[4].tool == 3);
    CHECK(f.hits[4].pos.x == Approx(-12.7).margin(1e-9)); // negative X
    CHECK(f.hits[4].pos.y == Approx(6.35).margin(1e-9));

    // Conversion: one circle per hit, radius = diameter/2, "plated" metadata.
    Document doc;
    const ExcellonImportResult res =
        excellonToDocument(doc, f, QStringLiteral("DRL"));
    INFO(res.error.toStdString());
    REQUIRE(res.ok);
    CHECK(res.hits == 5);
    CHECK(res.entities == 5);
    REQUIRE(doc.layerByName(QStringLiteral("DRL")) != nullptr);

    const auto circles = circlesOf(doc);
    REQUIRE(circles.size() == 5);
    CHECK(circles[0]->center().x == Approx(23.999952).margin(1e-9));
    CHECK(circles[0]->center().y == Approx(-9.100058).margin(1e-9));
    CHECK(circles[0]->radius() == Approx(0.149987).margin(1e-9));
    CHECK(circles[0]->extra().value(QLatin1String("plated")).toBool());
    CHECK(circles[3]->radius() == Approx(0.499999).margin(1e-9));
    CHECK(circles[4]->radius() == Approx(1.200023).margin(1e-9));
    CHECK_FALSE(circles[4]->extra().value(QLatin1String("plated")).toBool(true));

    // Drill hits render as FILLED disks (like gerbv), plated or not; a plain
    // CAD circle (no "plated" tag) stays an outline.
    RenderContext ctx;
    ctx.chordTolerance = 0.01;
    PrimitiveList drillPrims;
    circles[0]->buildPrimitives(ctx, drillPrims);
    REQUIRE(drillPrims.strokes.size() == 1);
    CHECK(drillPrims.strokes[0].filled);
    PrimitiveList unplatedPrims;
    circles[4]->buildPrimitives(ctx, unplatedPrims);
    REQUIRE(unplatedPrims.strokes.size() == 1);
    CHECK(unplatedPrims.strokes[0].filled);
    const CircleEntity plain({0.0, 0.0}, 1.0);
    PrimitiveList plainPrims;
    plain.buildPrimitives(ctx, plainPrims);
    REQUIRE(plainPrims.strokes.size() == 1);
    CHECK_FALSE(plainPrims.strokes[0].filled);

    // The whole import is ONE transaction: a single undo clears it.
    CHECK(doc.undo() == QStringLiteral("EXCELLONIMPORT"));
    CHECK(doc.entityCount() == 0);
    doc.redo();
    CHECK(doc.entityCount() == 5);
}

TEST_CASE("Excellon: LZ vs TZ diverge at the bounds", "[excellon]")
{
    // lz_inch.txt (INCH,LZ = leading zeros KEPT = TRAILING suppressed, default
    // inch format 2:4 => pad right to 6 digits):
    //   X1     -> "100000" -> 10.0000 in = 254 mm
    //   Y-1    -> -254 mm
    //   X001   -> "001000" ->  0.1000 in = 2.54 mm
    //   Y0094  -> "009400" ->  0.9400 in = 23.876 mm
    {
        const ExcellonFile f = parseGolden("lz_inch.txt");
        CHECK_FALSE(f.format.suppressLeading);
        CHECK(f.format.suppressionKnown);
        CHECK(f.format.intDigits == 2);
        CHECK(f.format.decDigits == 4);
        CHECK(f.tools.at(1).diameter == Approx(0.508).margin(1e-9));
        REQUIRE(f.hits.size() == 2);
        CHECK(f.hits[0].pos.x == Approx(254.0).margin(1e-9));
        CHECK(f.hits[0].pos.y == Approx(-254.0).margin(1e-9));
        CHECK(f.hits[1].pos.x == Approx(2.54).margin(1e-9));
        CHECK(f.hits[1].pos.y == Approx(23.876).margin(1e-9));
    }
    // The SAME digits under INCH,TZ (leading suppressed, 2:4): X1 is the
    // smallest increment, not ten inches.
    //   X1 -> 0.0001 in = 0.00254 mm ; Y-1 -> -0.00254 mm.
    {
        const ExcellonParseResult r = parseExcellonData(
            "M48\nINCH,TZ\nT1C0.02\n%\nT1\nX1Y-1\nM30\n");
        REQUIRE(r.ok);
        REQUIRE(r.file.hits.size() == 1);
        CHECK(r.file.hits[0].pos.x == Approx(0.00254).margin(1e-12));
        CHECK(r.file.hits[0].pos.y == Approx(-0.00254).margin(1e-12));
    }
}

TEST_CASE("Excellon: METRIC coordinates need no conversion", "[excellon]")
{
    // metric.txt (METRIC,TZ, default metric format 3:3):
    //   X12345 -> 12.345 mm ; Y-6789 -> -6.789 mm ; X1000 -> 1.000 mm
    //   (Y modal at -6.789). Tool diameters stay 1.5 / 3.175 mm as written.
    const ExcellonFile f = parseGolden("metric.txt");
    CHECK(f.unit == ExcellonUnit::Millimeters);
    CHECK(f.format.intDigits == 3);
    CHECK(f.format.decDigits == 3);
    CHECK(f.tools.at(1).diameter == Approx(1.5).margin(1e-12));
    CHECK(f.tools.at(2).diameter == Approx(3.175).margin(1e-12));
    REQUIRE(f.hits.size() == 2);
    CHECK(f.hits[0].pos.x == Approx(12.345).margin(1e-12));
    CHECK(f.hits[0].pos.y == Approx(-6.789).margin(1e-12));
    CHECK(f.hits[1].tool == 2);
    CHECK(f.hits[1].pos.x == Approx(1.0).margin(1e-12));
    CHECK(f.hits[1].pos.y == Approx(-6.789).margin(1e-12));
}

TEST_CASE("Excellon: explicit decimal coordinates bypass zero suppression",
          "[excellon]")
{
    // decimal.txt (INCH,TZ but every coordinate carries a '.'):
    //   X1.5 Y-0.25 -> (38.1, -6.35) mm ; Y0.75 -> (38.1, 19.05) mm.
    const ExcellonFile f = parseGolden("decimal.txt");
    CHECK(f.tools.at(1).diameter == Approx(2.54).margin(1e-9));
    REQUIRE(f.hits.size() == 2);
    CHECK(f.hits[0].pos.x == Approx(38.1).margin(1e-9));
    CHECK(f.hits[0].pos.y == Approx(-6.35).margin(1e-9));
    CHECK(f.hits[1].pos.x == Approx(38.1).margin(1e-9));
    CHECK(f.hits[1].pos.y == Approx(19.05).margin(1e-9));
}

TEST_CASE("Excellon: G85 slots parse without crash, warn, and are not drilled",
          "[excellon]")
{
    // g85_slot.txt (2:5 inch):
    //   X10000Y10000G85X30000Y10000 -> slot (2.54,2.54) -> (7.62,2.54) mm
    //   X50000Y50000                -> plain hit (12.7,12.7) mm
    //   G85X70000Y50000             -> slot with MODAL start (12.7,12.7)
    //                                  -> (17.78,12.7) mm
    const ExcellonFile f = parseGolden("g85_slot.txt");
    REQUIRE(f.hits.size() == 1);
    CHECK(f.hits[0].pos.x == Approx(12.7).margin(1e-9));
    REQUIRE(f.drillSlots.size() == 2);
    CHECK(f.drillSlots[0].from.x == Approx(2.54).margin(1e-9));
    CHECK(f.drillSlots[0].from.y == Approx(2.54).margin(1e-9));
    CHECK(f.drillSlots[0].to.x == Approx(7.62).margin(1e-9));
    CHECK(f.drillSlots[0].to.y == Approx(2.54).margin(1e-9));
    CHECK(f.drillSlots[1].from.x == Approx(12.7).margin(1e-9));
    CHECK(f.drillSlots[1].to.x == Approx(17.78).margin(1e-9));
    bool warned = false;
    for (const QString& w : f.warnings)
        warned = warned || w.contains(QStringLiteral("G85"));
    CHECK(warned);

    Document doc;
    const ExcellonImportResult res =
        excellonToDocument(doc, f, QStringLiteral("DRL"));
    REQUIRE(res.ok);
    CHECK(res.entities == 1); // slots are NOT silently drilled as hits
    bool convWarn = false;
    for (const QString& w : res.warnings)
        convWarn = convWarn || w.contains(QStringLiteral("slot"));
    CHECK(convWarn);
}

TEST_CASE("Excellon: missing LZ/TZ falls back to leading suppression with a "
          "warning", "[excellon]")
{
    // Bare "INCH" (default 2:4): X94488 -> 9.4488 in = 240.00. mm, and the
    // parser must say it guessed the suppression mode.
    const ExcellonParseResult r = parseExcellonData(
        "M48\nINCH\nT1C0.01\n%\nT1\nX94488Y0\nM30\n");
    REQUIRE(r.ok);
    CHECK_FALSE(r.file.format.suppressionKnown);
    REQUIRE(r.file.hits.size() == 1);
    CHECK(r.file.hits[0].pos.x == Approx(9.4488 * 25.4).margin(1e-9));
    bool warned = false;
    for (const QString& w : r.file.warnings)
        warned = warned || w.contains(QStringLiteral("LZ/TZ"));
    CHECK(warned);
}

TEST_CASE("Excellon: errors carry line numbers", "[excellon]")
{
    auto expectError = [](const char* data, const char* needle, int line) {
        const ExcellonParseResult r = parseExcellonData(QByteArray(data));
        INFO(data);
        REQUIRE_FALSE(r.ok);
        INFO(r.error.toStdString());
        CHECK(r.error.contains(QLatin1String(needle)));
        if (line > 0)
            CHECK(r.error.contains(QStringLiteral("line %1:").arg(line)));
    };

    // Drill hit with no tool selected.
    expectError("M48\nINCH,TZ\n%\nX100Y100\nM30\n", "no tool", 4);
    // Undefined tool selected in the body.
    expectError("M48\nINCH,TZ\nT1C0.01\n%\nT5\nX0Y0\nM30\n", "T5", 5);
    // Route mode would silently lose slots.
    expectError("M48\nINCH,TZ\nT1C0.01\n%\nT1\nG00X100\nM30\n", "route", 6);
    // Incremental input.
    expectError("M48\nINCH,TZ\nT1C0.01\n%\nT1\nG91\nX0Y0\nM30\n", "G91", 6);
    // Repeat code would silently multiply holes.
    expectError("M48\nINCH,TZ\nT1C0.01\n%\nT1\nX0Y0\nR2X100\nM30\n", "epeat", 7);
    // Coordinate before any unit declaration.
    expectError("M48\nT1C0.01\n%\nT1\nX100Y100\nM30\n", "INCH/METRIC", 5);
    // Coordinate wider than the declared format (2:5 = 7 digits max).
    expectError("M48\n;FILE_FORMAT=2:5\nINCH,TZ\nT1C0.01\n%\nT1\nX123456789\nM30\n",
                "bad coordinate", 7);
    // Header never closed.
    expectError("M48\nINCH,TZ\nT1C0.01\n", "never closed", 0);
    // Malformed FILE_FORMAT comment.
    expectError("M48\n;FILE_FORMAT=99\nINCH,TZ\n%\nM30\n", "FILE_FORMAT", 2);
}

TEST_CASE("Excellon: conversion refuses a diameter-less tool", "[excellon]")
{
    // A bare header "T1" defines the tool with no C field (warning), the body
    // may still reference it — but conversion must fail loudly, not emit
    // zero-radius circles.
    const ExcellonParseResult r = parseExcellonData(
        "M48\nINCH,TZ\nT1\n%\nT1\nX100Y100\nM30\n");
    REQUIRE(r.ok);
    bool warned = false;
    for (const QString& w : r.file.warnings)
        warned = warned || w.contains(QStringLiteral("diameter"));
    CHECK(warned);

    Document doc;
    const ExcellonImportResult res =
        excellonToDocument(doc, r.file, QStringLiteral("DRL"));
    REQUIRE_FALSE(res.ok);
    CHECK(res.error.contains(QStringLiteral("T1")));
    CHECK(res.error.contains(QStringLiteral("diameter")));
    CHECK(doc.entityCount() == 0);

    // Multi-token layer names are refused (command tokenizer law).
    const ExcellonParseResult ok = parseExcellonData(
        "M48\nINCH,TZ\nT1C0.01\n%\nT1\nX0Y0\nM30\n");
    REQUIRE(ok.ok);
    const ExcellonImportResult bad =
        excellonToDocument(doc, ok.file, QStringLiteral("bad layer"));
    REQUIRE_FALSE(bad.ok);
    CHECK(bad.error.contains(QStringLiteral("token")));
}

// ---------------------------------------------------------------------------
// Real Altium kits (skip when /home/lex/computer/pcb-ref is absent)
// ---------------------------------------------------------------------------

namespace {

struct DrrTool {
    double mm = 0.0; // hole size, from the "(0.3mm)" column
    int count = 0;
    bool plated = true;
};

// Parses the Altium .DRR NC drill report: fixed text table with lines like
//   "T1   12mil (0.3mm)   <tolerance>   Round   64   PTH   0.00inch (0.00mm)"
// plus a "Totals   <n>" footer. Independent ground truth for the .TXT parser.
std::map<int, DrrTool> parseDrr(const QString& path, int& total)
{
    std::map<int, DrrTool> out;
    total = -1;
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    while (!f.atEnd()) {
        const QStringList tok =
            QString::fromLatin1(f.readLine()).simplified().split(QLatin1Char(' '));
        if (tok.size() >= 2 && tok[0] == QLatin1String("Totals")) {
            total = tok[1].toInt();
            continue;
        }
        if (tok.size() < 5 || !tok[0].startsWith(QLatin1Char('T')))
            continue;
        bool numOk = false;
        const int num = tok[0].mid(1).toInt(&numOk);
        if (!numOk)
            continue;
        const int round = tok.indexOf(QStringLiteral("Round"));
        if (round < 0 || round + 2 >= tok.size())
            continue;
        DrrTool t;
        t.count = tok[round + 1].toInt();
        t.plated = tok[round + 2] == QLatin1String("PTH");
        for (const QString& s : tok) {
            if (s.startsWith(QLatin1Char('(')) && s.endsWith(QLatin1String("mm)"))) {
                t.mm = s.mid(1, s.size() - 4).toDouble();
                break;
            }
        }
        out[num] = t;
    }
    return out;
}

} // namespace

TEST_CASE("Excellon kits: per-tool hole counts match the .DRR report",
          "[excellon][kits]")
{
    if (!kitsPresent())
        SKIP("real drill kits not present on this machine");

    struct Kit {
        const char* dir;
        const char* prefix;
    };
    for (const Kit& kit : {Kit{"S5M0PCBA", "S5M0PCBA1"}, Kit{"S5M0PCBB", "S5M0PCBB1"}}) {
        const QString base = QStringLiteral("%1/%2/%3")
                                 .arg(QLatin1String(kKitRoot), QLatin1String(kit.dir),
                                      QLatin1String(kit.prefix));
        const ExcellonParseResult r = parseExcellon(base + QStringLiteral(".TXT"));
        INFO(kit.dir);
        INFO(r.error.toStdString());
        REQUIRE(r.ok);
        CHECK(r.file.sawEnd);
        CHECK(r.file.unit == ExcellonUnit::Inches);
        CHECK(r.file.format.fromComment); // ;FILE_FORMAT=2:5
        CHECK(r.file.format.intDigits == 2);
        CHECK(r.file.format.decDigits == 5);
        CHECK(r.file.format.suppressLeading); // INCH,TZ
        CHECK(r.file.drillSlots.empty());
        INFO(r.file.warnings.join(QStringLiteral(" | ")).toStdString());
        CHECK(r.file.warnings.isEmpty());

        int drrTotal = -1;
        const auto drr = parseDrr(base + QStringLiteral(".DRR"), drrTotal);
        REQUIRE(!drr.empty());
        REQUIRE(drrTotal > 0);

        // Same tool set, and per tool: hole count, plating and diameter
        // (the .DRR mm column is rounded, e.g. "12mil (0.3mm)" for 0.299974).
        std::map<int, int> counts;
        for (const ExcellonHit& h : r.file.hits)
            ++counts[h.tool];
        REQUIRE(r.file.tools.size() == drr.size());
        int sum = 0;
        for (const auto& [num, want] : drr) {
            INFO(kit.dir << " T" << num);
            REQUIRE(r.file.tools.count(num) == 1);
            CHECK(counts[num] == want.count);
            CHECK(r.file.tools.at(num).plated == want.plated);
            CHECK(r.file.tools.at(num).diameter == Approx(want.mm).margin(0.01));
            sum += want.count;
        }
        CHECK(sum == drrTotal);
        CHECK(int(r.file.hits.size()) == drrTotal);
    }

    // Spot arithmetic on the first hit of kit A ("T01" then "X94488Y-35827",
    // 2:5 INCH,TZ): X = 0.94488 in = 23.999952 mm, Y = -0.35827 in =
    // -9.100058 mm — decoded straight off the real Altium file.
    const ExcellonParseResult a = parseExcellon(
        QStringLiteral("%1/S5M0PCBA/S5M0PCBA1.TXT").arg(QLatin1String(kKitRoot)));
    REQUIRE(a.ok);
    REQUIRE(!a.file.hits.empty());
    CHECK(a.file.hits[0].tool == 1);
    CHECK(a.file.hits[0].pos.x == Approx(23.999952).margin(1e-9));
    CHECK(a.file.hits[0].pos.y == Approx(-9.100058).margin(1e-9));
}

TEST_CASE("Excellon kits: full conversion to circles with plating metadata",
          "[excellon][kits]")
{
    if (!kitsPresent())
        SKIP("real drill kits not present on this machine");

    // Kit A: .DRR says 182 holes, 180 plated (T1..T8) + 2 non-plated (T9,
    // 94mil = 2.400046 mm).
    const ExcellonParseResult r = parseExcellon(
        QStringLiteral("%1/S5M0PCBA/S5M0PCBA1.TXT").arg(QLatin1String(kKitRoot)));
    REQUIRE(r.ok);
    Document doc;
    const ExcellonImportResult res =
        excellonToDocument(doc, r.file, QStringLiteral("DRL"));
    INFO(res.error.toStdString());
    REQUIRE(res.ok);
    CHECK(res.hits == 182);
    CHECK(res.entities == 182);
    CHECK(doc.entityCount() == 182);
    REQUIRE(doc.layerByName(QStringLiteral("DRL")) != nullptr);

    int nonPlated = 0;
    for (const CircleEntity* c : circlesOf(doc)) {
        if (!c->extra().value(QLatin1String("plated")).toBool(true)) {
            ++nonPlated;
            CHECK(c->radius() == Approx(2.400046 / 2.0).margin(1e-9));
        }
    }
    CHECK(nonPlated == 2);

    // Single undo step for the whole import.
    CHECK(doc.undo() == QStringLiteral("EXCELLONIMPORT"));
    CHECK(doc.entityCount() == 0);
}
