// Excellon (NC drill) writer (G3 export stage).
//
// Three tiers:
//  - DIALECT checks: M48 / METRIC,TZ / ;TYPE sections / Tn C<dia> / modal
//    hits with EXPLICIT DECIMAL coordinates / M30 — the exact bytes;
//  - synthetic ROUND-TRIPS: build or import a document, export, re-import,
//    circles must match at 1e-6 mm (positions, radii, plating); tool table
//    regeneration cases (1e-4 grouping, plated-first ordering, role-based
//    plating default, multi-layer grouping, .vkd persistence);
//  - the TRUTH TEST on the real Altium kits: export -> re-import -> the
//    per-tool hole counts and diameters must equal the .DRR report (the
//    same independent ground truth test_excellon.cpp uses), every hit
//    position must survive at 1e-6 mm, and gerbv renders the ORIGINAL and
//    the EXPORTED drill file identically (dhash < 30/1024, ink delta <=
//    1 point — our renderer stays out of the loop). SKIPs without the
//    kits or without gerbv.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include "doc/Document.h"
#include "doc/Entities.h"
#include "io/ExcellonIo.h"
#include "io/ExcellonWriter.h"
#include "io/NativeStore.h"

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

// Adds one drill circle (importer-shaped: "plated" + "tool" tags).
void addHole(Document& doc, LayerId layer, const Vec2d& pos, double dia,
             bool plated, const char* tool = nullptr)
{
    auto c = std::make_unique<CircleEntity>(pos, dia * 0.5);
    c->setLayerId(layer);
    c->setExtraValue(QStringLiteral("plated"), plated);
    if (tool)
        c->setExtraValue(QStringLiteral("tool"), QLatin1String(tool));
    doc.addEntity(std::move(c));
}

QByteArray exportLayers(const Document& doc, const QStringList& layers,
                        ExcellonExportResult* resOut = nullptr)
{
    QByteArray bytes;
    const ExcellonExportResult res = writeExcellon(doc, layers, bytes);
    INFO(res.error.toStdString());
    REQUIRE(res.ok);
    if (resOut)
        *resOut = res;
    return bytes;
}

// Re-imports exported bytes into a fresh document (layer "RT").
Document reimport(const QByteArray& bytes)
{
    const ExcellonParseResult r = parseExcellonData(bytes);
    INFO(r.error.toStdString());
    REQUIRE(r.ok);
    Document doc;
    const ExcellonImportResult res =
        excellonToDocument(doc, r.file, QStringLiteral("RT"));
    INFO(res.error.toStdString());
    REQUIRE(res.ok);
    return doc;
}

struct HoleKey {
    double x, y, dia;
    bool plated;
};

// Every drill circle of the document, sorted for order-free comparison.
std::vector<HoleKey> sortedHoles(const Document& doc)
{
    std::vector<HoleKey> out;
    for (const EntityId id : doc.drawOrder()) {
        const auto* c = dynamic_cast<const CircleEntity*>(doc.entity(id));
        if (!c)
            continue;
        out.push_back({c->center().x, c->center().y, c->radius() * 2.0,
                       c->extra().value(QLatin1String("plated")).toBool()});
    }
    std::sort(out.begin(), out.end(), [](const HoleKey& a, const HoleKey& b) {
        if (a.dia != b.dia)
            return a.dia < b.dia;
        if (a.plated != b.plated)
            return a.plated;
        if (a.x != b.x)
            return a.x < b.x;
        return a.y < b.y;
    });
    return out;
}

// "" when both documents hold the same drill circles within tol (mm).
QString compareHoles(const Document& a, const Document& b, double tol)
{
    const auto ha = sortedHoles(a);
    const auto hb = sortedHoles(b);
    if (ha.size() != hb.size())
        return QStringLiteral("hole count %1 != %2").arg(ha.size()).arg(hb.size());
    for (size_t i = 0; i < ha.size(); ++i) {
        if (std::fabs(ha[i].x - hb[i].x) > tol ||
            std::fabs(ha[i].y - hb[i].y) > tol)
            return QStringLiteral("hole %1 moved").arg(i);
        if (std::fabs(ha[i].dia - hb[i].dia) > tol)
            return QStringLiteral("hole %1 diameter %2 != %3")
                .arg(i)
                .arg(ha[i].dia)
                .arg(hb[i].dia);
        if (ha[i].plated != hb[i].plated)
            return QStringLiteral("hole %1 plating differs").arg(i);
    }
    return QString();
}

// ---------------------------------------------------------------------------
// .DRR ground truth (same parser as test_excellon.cpp).
// ---------------------------------------------------------------------------

struct DrrTool {
    double mm = 0.0;
    int count = 0;
    bool plated = true;
};

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

// ---------------------------------------------------------------------------
// gerbv-side comparison (same logic as test_gerber_export.cpp: binarized ink
// masks cropped to the ink bbox, 32x32 dhash + ink-density delta).
// ---------------------------------------------------------------------------

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
    QImage img;
    double ink = 0.0;
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
        for (int x = 0; x < gray.width(); ++x) {
            if (line[x] > 48) {
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
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

} // namespace

// ---------------------------------------------------------------------------
// Dialect
// ---------------------------------------------------------------------------

TEST_CASE("Excellon writer: dialect — header, sections, decimal modal hits",
          "[excellon][export]")
{
    Document doc;
    const LayerId lid = doc.ensureLayer(QStringLiteral("DRL"));
    {
        TransactionScope tx(doc, QStringLiteral("T"));
        addHole(doc, lid, {1.5, -2.25}, 0.3, true);
        addHole(doc, lid, {1.5, 4.0}, 0.3, true);   // X modal
        addHole(doc, lid, {3.0, 4.0}, 0.3, true);   // Y modal
        addHole(doc, lid, {3.0, 4.0}, 0.3, true);   // duplicate: restate both
        addHole(doc, lid, {0.0, 0.0}, 2.4, false);  // NPTH
        tx.commit();
    }
    ExcellonExportResult res;
    const QByteArray bytes = exportLayers(doc, {QStringLiteral("DRL")}, &res);
    CHECK(res.holes == 5);
    CHECK(res.tools == 2);
    CHECK(res.skipped == 0);

    const QString text = QString::fromUtf8(bytes);
    CHECK(text.startsWith(QStringLiteral("M48\n")));
    CHECK(text.contains(QStringLiteral(";GenerationSoftware,VikiCAD,")));
    CHECK(text.count(QStringLiteral("METRIC,TZ")) == 1);
    // Plated section first, tools numbered from T1, NPTH after.
    CHECK(text.contains(QStringLiteral(";TYPE=PLATED\nT1C0.3\n")));
    CHECK(text.contains(QStringLiteral(";TYPE=NON_PLATED\nT2C2.4\n")));
    CHECK(text.indexOf(QStringLiteral(";TYPE=PLATED")) <
          text.indexOf(QStringLiteral(";TYPE=NON_PLATED")));
    CHECK(text.contains(QStringLiteral("\n%\n")));
    CHECK(text.trimmed().endsWith(QStringLiteral("M30")));

    // Explicit decimal coordinates, modal per axis, full restatement on the
    // duplicate hole and after a tool change.
    CHECK(text.contains(QStringLiteral("\nT1\nX1.5Y-2.25\nY4.0\nX3.0\nX3.0Y4.0\n")));
    CHECK(text.contains(QStringLiteral("\nT2\nX0.0Y0.0\n")));
    // Every coordinate is decimal — no bare integer that would depend on the
    // consumer's zero-suppression arithmetic.
    for (const QString& line : text.split(QLatin1Char('\n')))
        if (line.startsWith(QLatin1Char('X')) || line.startsWith(QLatin1Char('Y'))) {
            INFO(line.toStdString());
            CHECK(line.contains(QLatin1Char('.')));
        }
}

// ---------------------------------------------------------------------------
// Round-trips + tool regeneration
// ---------------------------------------------------------------------------

TEST_CASE("Excellon writer: synthetic round-trip at 1e-6", "[excellon][export]")
{
    Document doc;
    const LayerId lid = doc.ensureLayer(QStringLiteral("DRL"));
    {
        TransactionScope tx(doc, QStringLiteral("T"));
        addHole(doc, lid, {23.999952, -9.100058}, 0.299974, true);
        addHole(doc, lid, {-12.7, 6.35}, 0.299974, true);
        addHole(doc, lid, {0.0, 0.0}, 1.016, true);
        addHole(doc, lid, {100.5, 200.25}, 2.400046, false);
        addHole(doc, lid, {-0.001, -0.001}, 2.400046, false);
        tx.commit();
    }
    const QByteArray bytes = exportLayers(doc, {QStringLiteral("DRL")});
    Document doc2 = reimport(bytes);
    const QString diff = compareHoles(doc, doc2, 1e-6);
    INFO(diff.toStdString());
    CHECK(diff.isEmpty());

    // The importer's own metadata comes back too (tool table on the layer).
    const Layer* l = doc2.layerByName(QStringLiteral("RT"));
    REQUIRE(l != nullptr);
    CHECK(!l->camMeta.value(QLatin1String("tools")).toObject().isEmpty());
}

TEST_CASE("Excellon writer: golden import round-trips exactly",
          "[excellon][export]")
{
    // tz_inch.txt: the Altium-dialect golden (2:5 inch, sections, modal and
    // negative coordinates). Import -> export -> re-import must preserve
    // every hole at 1e-6 mm, and the exported bytes must be stable across a
    // .vkd save/load (the tags and camMeta persist).
    const ExcellonParseResult r = parseExcellon(goldenPath("tz_inch.txt"));
    INFO(r.error.toStdString());
    REQUIRE(r.ok);
    Document doc;
    REQUIRE(excellonToDocument(doc, r.file, QStringLiteral("DRL")).ok);

    const QByteArray bytes = exportLayers(doc, {QStringLiteral("DRL")});
    const Document doc2 = reimport(bytes);
    const QString diff = compareHoles(doc, doc2, 1e-6);
    INFO(diff.toStdString());
    CHECK(diff.isEmpty());

    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString vkd = tmp.filePath(QStringLiteral("drill.vkd"));
    QString err;
    REQUIRE(NativeStore::save(doc, vkd, err));
    const auto loaded = NativeStore::load(vkd, err);
    REQUIRE(loaded != nullptr);
    const QByteArray bytes2 = exportLayers(*loaded, {QStringLiteral("DRL")});
    CHECK(bytes2 == bytes);
}

TEST_CASE("Excellon writer: tool regeneration — 1e-4 grouping, plated first, "
          "ascending diameters", "[excellon][export]")
{
    Document doc;
    const LayerId lid = doc.ensureLayer(QStringLiteral("DRL"));
    {
        TransactionScope tx(doc, QStringLiteral("T"));
        addHole(doc, lid, {0.0, 0.0}, 1.0, false);       // NPTH — sorts last
        addHole(doc, lid, {1.0, 0.0}, 0.5, true);
        addHole(doc, lid, {2.0, 0.0}, 0.500004, true);   // same tool (< 1e-4)
        addHole(doc, lid, {3.0, 0.0}, 0.5002, true);     // distinct tool
        addHole(doc, lid, {4.0, 0.0}, 1.0, true);        // plated 1.0 != NPTH 1.0
        tx.commit();
    }
    ExcellonExportResult res;
    const QString text =
        QString::fromUtf8(exportLayers(doc, {QStringLiteral("DRL")}, &res));
    CHECK(res.tools == 4);
    // Plated ascending (0.5, 0.5002, 1.0) then NPTH (1.0). The 0.5-group
    // diameter is the FIRST circle's, full precision (not the 1e-4 bucket).
    CHECK(text.contains(QStringLiteral(";TYPE=PLATED\nT1C0.5\nT2C0.5002\nT3C1.0\n"
                                       ";TYPE=NON_PLATED\nT4C1.0\n%")));
    // The near-duplicate diameter drilled with T1, at its own position.
    CHECK(text.contains(QStringLiteral("\nT1\nX1.0Y0.0\nX2.0\n")));
}

TEST_CASE("Excellon writer: empty layer list selects the Drill-role layers, "
          "plating defaults to the role", "[excellon][export]")
{
    Document doc;
    const LayerId pth = doc.ensureLayer(QStringLiteral("Drill"));
    const LayerId npth = doc.ensureLayer(QStringLiteral("Drill-NPTH"));
    const LayerId other = doc.ensureLayer(QStringLiteral("Copper"));
    doc.setLayerGerberRole(pth, QStringLiteral("Drill"));
    doc.setLayerGerberRole(npth, QStringLiteral("Drill-NPTH"));
    {
        TransactionScope tx(doc, QStringLiteral("T"));
        // Bare CAD circles: no "plated" tag — the layer's role decides.
        auto a = std::make_unique<CircleEntity>(Vec2d{1.0, 1.0}, 0.5);
        a->setLayerId(pth);
        doc.addEntity(std::move(a));
        auto b = std::make_unique<CircleEntity>(Vec2d{5.0, 5.0}, 1.5);
        b->setLayerId(npth);
        doc.addEntity(std::move(b));
        auto c = std::make_unique<CircleEntity>(Vec2d{9.0, 9.0}, 2.0);
        c->setLayerId(other); // not a drill layer: must NOT be exported
        doc.addEntity(std::move(c));
        tx.commit();
    }
    ExcellonExportResult res;
    const QString text = QString::fromUtf8(exportLayers(doc, {}, &res));
    CHECK(res.holes == 2);
    CHECK(res.tools == 2);
    CHECK(text.contains(QStringLiteral(";TYPE=PLATED\nT1C1.0\n")));
    CHECK(text.contains(QStringLiteral(";TYPE=NON_PLATED\nT2C3.0\n")));
    CHECK(!text.contains(QStringLiteral("X9.0")));

    // The same grouped export, with the layers named explicitly.
    ExcellonExportResult res2;
    QByteArray bytes2;
    const ExcellonExportResult r2 = writeExcellon(
        doc, {QStringLiteral("Drill"), QStringLiteral("Drill-NPTH")}, bytes2);
    REQUIRE(r2.ok);
    CHECK(QString::fromUtf8(bytes2) == text);
}

TEST_CASE("Excellon writer: errors, skips and empty exports", "[excellon][export]")
{
    Document doc;
    QByteArray bytes;

    // Unknown layer.
    ExcellonExportResult bad =
        writeExcellon(doc, {QStringLiteral("Nope")}, bytes);
    CHECK(!bad.ok);
    CHECK(bad.error.contains(QStringLiteral("Nope")));

    // Empty list without any Drill-role layer.
    bad = writeExcellon(doc, {}, bytes);
    CHECK(!bad.ok);
    CHECK(bad.error.contains(QStringLiteral("Drill")));

    // Empty layer: header-only file, warned, and our parser accepts it.
    doc.ensureLayer(QStringLiteral("DRL"));
    ExcellonExportResult res;
    const QByteArray out = exportLayers(doc, {QStringLiteral("DRL")}, &res);
    CHECK(res.holes == 0);
    bool warned = false;
    for (const QString& w : res.warnings)
        warned = warned || w.contains(QStringLiteral("no drill hits"));
    CHECK(warned);
    CHECK(parseExcellonData(out).ok);

    // A non-circle entity on the drill layer is skipped with a warning.
    {
        TransactionScope tx(doc, QStringLiteral("T"));
        auto ln = std::make_unique<LineEntity>(Vec2d{0.0, 0.0}, Vec2d{1.0, 1.0});
        ln->setLayerId(doc.layerByName(QStringLiteral("DRL"))->id);
        doc.addEntity(std::move(ln));
        addHole(doc, doc.layerByName(QStringLiteral("DRL"))->id, {1.0, 1.0},
                0.5, true);
        tx.commit();
    }
    const QByteArray out2 = exportLayers(doc, {QStringLiteral("DRL")}, &res);
    CHECK(res.holes == 1);
    CHECK(res.skipped == 1);
    warned = false;
    for (const QString& w : res.warnings)
        warned = warned || w.contains(QStringLiteral("no drill image"));
    CHECK(warned);

    // Out-of-range coordinate = hard error (same guard as the Gerber writer).
    Document far;
    const LayerId fid = far.ensureLayer(QStringLiteral("DRL"));
    {
        TransactionScope tx(far, QStringLiteral("T"));
        addHole(far, fid, {10000.5, 0.0}, 0.5, true);
        tx.commit();
    }
    bad = writeExcellon(far, {QStringLiteral("DRL")}, bytes);
    CHECK(!bad.ok);
    CHECK(bad.error.contains(QStringLiteral("9999.999999")));
}

// ---------------------------------------------------------------------------
// The truth test: real kits — .DRR equality after re-import + gerbv render
// ---------------------------------------------------------------------------

TEST_CASE("Excellon writer: kit export matches the .DRR and renders like the "
          "original under gerbv", "[excellon][export][kits]")
{
    if (!kitsPresent()) {
        SKIP("real drill kits not present on this machine");
    }
    const QString gerbv = QStandardPaths::findExecutable(QStringLiteral("gerbv"));
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());

    struct Kit {
        const char* dir;
        const char* prefix;
    };
    for (const Kit& kit : {Kit{"S5M0PCBA", "S5M0PCBA1"}, Kit{"S5M0PCBB", "S5M0PCBB1"}}) {
        INFO(kit.dir);
        const QString base = QStringLiteral("%1/%2/%3")
                                 .arg(QLatin1String(kKitRoot), QLatin1String(kit.dir),
                                      QLatin1String(kit.prefix));
        const ExcellonParseResult r = parseExcellon(base + QStringLiteral(".TXT"));
        INFO(r.error.toStdString());
        REQUIRE(r.ok);
        Document doc;
        REQUIRE(excellonToDocument(doc, r.file, QStringLiteral("DRL")).ok);

        const QString exported = tmp.filePath(QLatin1String(kit.prefix) +
                                              QStringLiteral(".exp.txt"));
        const ExcellonExportResult res =
            exportExcellon(doc, {QStringLiteral("DRL")}, exported);
        INFO(res.error.toStdString());
        REQUIRE(res.ok);
        CHECK(res.skipped == 0);

        // Semantic round-trip: every hole survives at 1e-6 mm.
        QFile ef(exported);
        REQUIRE(ef.open(QIODevice::ReadOnly));
        const QByteArray bytes = ef.readAll();
        const Document doc2 = reimport(bytes);
        const QString diff = compareHoles(doc, doc2, 1e-6);
        INFO(diff.toStdString());
        CHECK(diff.isEmpty());

        // .DRR ground truth on the RE-IMPORTED document (DRILLREPORT's
        // grouping rule: diameter x plating): same tool set, same per-tool
        // hole counts, diameters within the report's rounding (0.01 mm).
        int drrTotal = -1;
        const auto drr = parseDrr(base + QStringLiteral(".DRR"), drrTotal);
        REQUIRE(!drr.empty());
        REQUIRE(drrTotal > 0);
        std::map<std::pair<qint64, bool>, int> groups; // (dia 1e-6, plated) -> n
        std::map<std::pair<qint64, bool>, double> groupDia;
        for (const HoleKey& h : sortedHoles(doc2)) {
            const std::pair<qint64, bool> key{qint64(std::llround(h.dia * 1e6)),
                                              h.plated};
            ++groups[key];
            groupDia[key] = h.dia;
        }
        REQUIRE(groups.size() == drr.size());
        int matchedTotal = 0;
        for (const auto& [num, want] : drr) {
            INFO(kit.dir << " .DRR T" << num);
            int found = 0;
            for (const auto& [key, n] : groups)
                if (key.second == want.plated &&
                    std::fabs(groupDia[key] - want.mm) <= 0.01 && n == want.count)
                    ++found;
            CHECK(found == 1);
            matchedTotal += want.count;
        }
        CHECK(matchedTotal == drrTotal);
        CHECK(res.holes == drrTotal);

        // The gerbv truth: the exported drill file renders like the original.
        if (gerbv.isEmpty())
            continue; // parser-level checks above still ran
        const QString refPng = exported + QStringLiteral(".ref.png");
        const QString gotPng = exported + QStringLiteral(".got.png");
        REQUIRE(renderWithGerbv(gerbv, base + QStringLiteral(".TXT"), refPng));
        REQUIRE(renderWithGerbv(gerbv, exported, gotPng));
        const InkMask ref = inkMask(refPng);
        const InkMask got = inkMask(gotPng);
        REQUIRE(!ref.img.isNull());
        REQUIRE(!got.img.isNull());
        const int dist = dhashDistance(ref.img, got.img);
        const double inkDelta = std::fabs(ref.ink - got.ink) * 100.0;
        INFO("dhash=" << dist << " ink-delta=" << inkDelta << "%");
        CHECK(dist < 30);
        CHECK(inkDelta <= 1.0);
    }
}
