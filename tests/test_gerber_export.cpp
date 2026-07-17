// Gerber RS-274X writer (G3 export stage).
//
// Three tiers:
//  - synthetic golden ROUND-TRIPS: each committed construct is imported,
//    exported, re-imported, and the two documents must match at 1e-6 mm;
//  - aperture REGENERATION cases: edited width -> new C entry, uniform
//    insert scale folded into the definition, off-axis/non-uniform pads ->
//    outline-macro fallback with a warning, dedup from D10;
//  - the TRUTH TEST: for every Gerber layer of the two real Altium kits,
//    gerbv renders the ORIGINAL and the EXPORTED file; the normalized ink
//    masks must agree (dhash < 30/1024, ink delta <= 1 point). Our renderer
//    is OUT of the loop: gerbv judges our writing. SKIPs without the kits
//    or without gerbv.

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

#include "doc/Annotations.h"
#include "doc/Block.h"
#include "doc/Document.h"
#include "doc/EntitiesEx.h"
#include "io/GerberIo.h"
#include "io/GerberWriter.h"
#include "io/NativeStore.h"

using namespace viki;
using Catch::Approx;

namespace {

QString goldenPath(const char* name)
{
    return QStringLiteral(VIKICAD_GOLDEN_DIR "/gerber/") + QLatin1String(name);
}

const char* kKitRoot = "/home/lex/computer/pcb-ref";

bool kitsPresent()
{
    return QDir(QLatin1String(kKitRoot) + QLatin1String("/S5M0PCBA")).exists() &&
           QDir(QLatin1String(kKitRoot) + QLatin1String("/S5M0PCBB")).exists();
}

// Import a parsed Gerber file into a fresh document on layer "L".
GerberFile parseFileOrFail(const QString& path)
{
    const GerberParseResult r = parseGerber(path);
    INFO(path.toStdString() << ": " << r.error.toStdString());
    REQUIRE(r.ok);
    return r.file;
}

void importInto(Document& doc, const GerberFile& f)
{
    const GerberImportResult r = gerberToDocument(doc, f, QStringLiteral("L"));
    INFO(r.error.toStdString());
    REQUIRE(r.ok);
}

QByteArray exportLayer(const Document& doc, GerberExportResult* resOut = nullptr)
{
    QByteArray bytes;
    const GerberExportResult res =
        writeGerberLayer(doc, QStringLiteral("L"), bytes);
    INFO(res.error.toStdString());
    REQUIRE(res.ok);
    if (resOut)
        *resOut = res;
    return bytes;
}

Document reimport(const QByteArray& bytes)
{
    const GerberParseResult r = parseGerberData(bytes);
    INFO(r.error.toStdString());
    REQUIRE(r.ok);
    Document doc;
    importInto(doc, r.file);
    return doc;
}

// ---------------------------------------------------------------------------
// Semantic document comparison (returns "" when equal within tolerances).
// tol: hard positional tolerance (vertices, ring points, flash origins).
// softTol: tessellation-sensitive geometry (block footprint rings, bulges) —
// re-tessellating an aperture whose parameters were rounded to 1e-6 mm moves
// ring points by less than 1e-6, but rotations re-applied from 6-decimal
// degrees justify a little slack.
// ---------------------------------------------------------------------------

std::vector<std::vector<Vec2d>> insertWorldRings(const Document& doc,
                                                 const InsertEntity& ins)
{
    std::vector<std::vector<Vec2d>> out;
    const BlockDef* def = doc.blockByName(ins.blockName);
    if (!def)
        return out;
    const Xform2d xf = ins.insertXform(def->basePoint);
    for (const auto& sub : def->entities) {
        const auto* h = dynamic_cast<const HatchEntity*>(sub.get());
        if (!h)
            continue;
        for (const auto& ring : h->rings) {
            std::vector<Vec2d> r;
            r.reserve(ring.size());
            for (const Vec2d& p : ring)
                r.push_back(xf.apply(p));
            out.push_back(std::move(r));
        }
    }
    return out;
}

QString compareDocs(const Document& a, const Document& b, double tol, double softTol)
{
    const auto& da = a.drawOrder();
    const auto& db = b.drawOrder();
    if (da.size() != db.size())
        return QStringLiteral("entity count %1 != %2").arg(da.size()).arg(db.size());
    for (size_t i = 0; i < da.size(); ++i) {
        const Entity* ea = a.entity(da[i]);
        const Entity* eb = b.entity(db[i]);
        const QString at = QStringLiteral("entity %1 (#%2/#%3): ")
                               .arg(i)
                               .arg(da[i])
                               .arg(db[i]);
        if (QLatin1String(ea->typeName()) != QLatin1String(eb->typeName()))
            return at + QStringLiteral("type %1 != %2")
                            .arg(QLatin1String(ea->typeName()),
                                 QLatin1String(eb->typeName()));
        const QString ga = ea->extra().value(QLatin1String("gpol")).toString();
        const QString gb = eb->extra().value(QLatin1String("gpol")).toString();
        if (ga != gb)
            return at + QStringLiteral("polarity '%1' != '%2'").arg(ga, gb);

        if (const auto* pa = dynamic_cast<const PolylineEntity*>(ea)) {
            const auto* pb = dynamic_cast<const PolylineEntity*>(eb);
            if (std::fabs(pa->width() - pb->width()) > tol)
                return at + QStringLiteral("width %1 != %2")
                                .arg(pa->width())
                                .arg(pb->width());
            if (pa->vertices().size() != pb->vertices().size())
                return at + QStringLiteral("vertex count %1 != %2")
                                .arg(pa->vertices().size())
                                .arg(pb->vertices().size());
            for (size_t k = 0; k < pa->vertices().size(); ++k) {
                const PolyVertex& va = pa->vertices()[k];
                const PolyVertex& vb = pb->vertices()[k];
                if (!nearEqual(va.pos, vb.pos, tol))
                    return at + QStringLiteral("vertex %1 moved").arg(k);
                if (std::fabs(va.bulge - vb.bulge) > softTol)
                    return at + QStringLiteral("bulge %1: %2 != %3")
                                    .arg(k)
                                    .arg(va.bulge)
                                    .arg(vb.bulge);
            }
            continue;
        }
        if (const auto* ia = dynamic_cast<const InsertEntity*>(ea)) {
            const auto* ib = dynamic_cast<const InsertEntity*>(eb);
            if (!nearEqual(ia->position, ib->position, tol))
                return at + QStringLiteral("flash position moved");
            const auto ra = insertWorldRings(a, *ia);
            const auto rb = insertWorldRings(b, *ib);
            if (ra.size() != rb.size())
                return at + QStringLiteral("footprint ring count %1 != %2")
                                .arg(ra.size())
                                .arg(rb.size());
            for (size_t k = 0; k < ra.size(); ++k) {
                if (ra[k].size() == rb[k].size()) {
                    for (size_t m = 0; m < ra[k].size(); ++m)
                        if (!nearEqual(ra[k][m], rb[k][m], softTol))
                            return at +
                                   QStringLiteral(
                                       "footprint ring %1 point %2 moved")
                                       .arg(k)
                                       .arg(m);
                    continue;
                }
                // Different point counts = the SAME analytic shape got
                // re-tessellated at a different radius (e.g. a scaled circle
                // aperture). Compare shape metrics: bbox within two chord
                // tolerances (points sit ON the true contour, the bbox can
                // sag by the 0.001 mm ring tolerance) and area within 1%.
                BBox2d boxA, boxB;
                for (const Vec2d& p : ra[k])
                    boxA.expand(p);
                for (const Vec2d& p : rb[k])
                    boxB.expand(p);
                const double geoTol = softTol + 0.002;
                if (!nearEqual(boxA.min, boxB.min, geoTol) ||
                    !nearEqual(boxA.max, boxB.max, geoTol))
                    return at +
                           QStringLiteral("footprint ring %1 bbox moved").arg(k);
                const auto area = [](const std::vector<Vec2d>& ring) {
                    double s = 0.0;
                    for (size_t m = 0; m < ring.size(); ++m)
                        s += ring[m].cross(ring[(m + 1) % ring.size()]);
                    return std::fabs(s) / 2.0;
                };
                const double aA = area(ra[k]), aB = area(rb[k]);
                if (std::fabs(aA - aB) > 0.01 * std::max({aA, aB, 1e-9}))
                    return at +
                           QStringLiteral("footprint ring %1 area %2 != %3")
                               .arg(k)
                               .arg(aA)
                               .arg(aB);
            }
            continue;
        }
        if (const auto* ha = dynamic_cast<const HatchEntity*>(ea)) {
            const auto* hb = dynamic_cast<const HatchEntity*>(eb);
            if (ha->rings.size() != hb->rings.size())
                return at + QStringLiteral("ring count %1 != %2")
                                .arg(ha->rings.size())
                                .arg(hb->rings.size());
            for (size_t k = 0; k < ha->rings.size(); ++k) {
                if (ha->rings[k].size() != hb->rings[k].size())
                    return at + QStringLiteral("ring %1 size %2 != %3")
                                    .arg(k)
                                    .arg(ha->rings[k].size())
                                    .arg(hb->rings[k].size());
                for (size_t m = 0; m < ha->rings[k].size(); ++m)
                    if (!nearEqual(ha->rings[k][m], hb->rings[k][m], tol))
                        return at + QStringLiteral("ring %1 point %2 moved")
                                        .arg(k)
                                        .arg(m);
            }
            continue;
        }
        // Anything else: same bounds is the best generic statement.
        const BBox2d ba = a.entityBounds(*ea);
        const BBox2d bb = b.entityBounds(*eb);
        if (!nearEqual(ba.min, bb.min, tol) || !nearEqual(ba.max, bb.max, tol))
            return at + QStringLiteral("bounds moved");
    }
    return QString();
}

void checkRoundTrip(const char* golden, double tol = 1e-6, double softTol = 1e-5)
{
    const GerberFile f = parseFileOrFail(goldenPath(golden));
    Document doc;
    importInto(doc, f);
    const QByteArray bytes = exportLayer(doc);
    const Document doc2 = reimport(bytes);
    const QString diff = compareDocs(doc, doc2, tol, softTol);
    INFO(golden << ": " << diff.toStdString());
    CHECK(diff.isEmpty());
}

// ---------------------------------------------------------------------------
// gerbv-side comparison (the reusable truth comparator: same dhash/ink logic
// as scripts/gerber-ref-diff.sh, tight thresholds since both images come
// from gerbv).
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

// Binarized ink mask cropped to its ink bounding box, resized to 256x256.
// isNull() when the layer draws nothing.
struct InkMask {
    QImage img;   // 256x256 grayscale
    double ink = 0.0; // fraction of bright pixels
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
            const bool on = line[x] > 48;
            if (on) {
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
        }
    }
    if (maxX < 0)
        return out; // empty
    QImage mask(gray.width(), gray.height(), QImage::Format_Grayscale8);
    for (int y = 0; y < gray.height(); ++y) {
        const uchar* in = gray.constScanLine(y);
        uchar* dst = mask.scanLine(y);
        for (int x = 0; x < gray.width(); ++x)
            dst[x] = in[x] > 48 ? 255 : 0;
    }
    const QImage cropped =
        mask.copy(QRect(QPoint(minX, minY), QPoint(maxX, maxY)));
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
// Synthetic golden round-trips (each committed construct)
// ---------------------------------------------------------------------------

TEST_CASE("Gerber writer: golden constructs round-trip at 1e-6", "[gerber][export]")
{
    checkRoundTrip("draw_round.gbr");
    checkRoundTrip("arc_g75.gbr");
    checkRoundTrip("g74_single.gbr");
    checkRoundTrip("flash_std.gbr");
    checkRoundTrip("macro_roundedrect.gbr");
    checkRoundTrip("region.gbr");
    checkRoundTrip("polarity_order.gbr");
    checkRoundTrip("lpc_redraw.gbr");
    checkRoundTrip("mm_file.gbr");
    checkRoundTrip("zeros_leading.gbr");
    checkRoundTrip("zeros_trailing.gbr");
    checkRoundTrip("attrs_x2.gbr");
}

TEST_CASE("Gerber writer: dialect header and polarity order", "[gerber][export]")
{
    const GerberFile f = parseFileOrFail(goldenPath("polarity_order.gbr"));
    Document doc;
    importInto(doc, f);
    const QByteArray bytes = exportLayer(doc);
    const QString text = QString::fromUtf8(bytes);

    // Modern metric dialect, one statement each.
    CHECK(text.count(QStringLiteral("%FSLAX46Y46*%")) == 1);
    CHECK(text.count(QStringLiteral("%MOMM*%")) == 1);
    CHECK(text.contains(QStringLiteral("%TF.GenerationSoftware,VikiCAD,")));
    CHECK(text.contains(QStringLiteral("G75*")));
    CHECK(text.trimmed().endsWith(QStringLiteral("M02*")));

    // LPD flash, then LPC region, then a NEW %LPD*% for the redraw trace —
    // the paint order is the semantics.
    const int lpc = text.indexOf(QStringLiteral("%LPC*%"));
    const int lpd = text.indexOf(QStringLiteral("%LPD*%"), lpc);
    REQUIRE(lpc > 0);
    REQUIRE(lpd > lpc);
    const int flash = text.indexOf(QStringLiteral("D03*"));
    const int region = text.indexOf(QStringLiteral("G36*"));
    CHECK(flash < lpc);
    CHECK(region > lpc);
    CHECK(region < lpd);
}

// ---------------------------------------------------------------------------
// Aperture regeneration
// ---------------------------------------------------------------------------

TEST_CASE("Gerber writer: aperture dedup numbers from D10", "[gerber][export]")
{
    Document doc;
    doc.ensureLayer(QStringLiteral("L"));
    const int64_t lid = doc.layerByName(QStringLiteral("L"))->id;
    {
        TransactionScope tx(doc, QStringLiteral("T"));
        for (int i = 0; i < 3; ++i) {
            auto pl = std::make_unique<PolylineEntity>(
                std::vector<PolyVertex>{{{0.0, double(i)}, 0.0},
                                        {{10.0, double(i)}, 0.0}},
                false);
            pl->setWidth(0.3);
            pl->setLayerId(lid);
            doc.addEntity(std::move(pl));
        }
        auto pl = std::make_unique<PolylineEntity>(
            std::vector<PolyVertex>{{{0.0, 5.0}, 0.0}, {{10.0, 5.0}, 0.0}}, false);
        pl->setWidth(0.5);
        pl->setLayerId(lid);
        doc.addEntity(std::move(pl));
        tx.commit();
    }
    GerberExportResult res;
    const QByteArray bytes = exportLayer(doc, &res);
    const QString text = QString::fromUtf8(bytes);
    CHECK(res.apertures == 2); // 0.3 deduplicated, 0.5 separate
    CHECK(text.contains(QStringLiteral("%ADD10C,0.3*%")));
    CHECK(text.contains(QStringLiteral("%ADD11C,0.5*%")));
    CHECK(res.entities == 4);
}

TEST_CASE("Gerber writer: unedited rect-aperture draw reuses the R definition",
          "[gerber][export]")
{
    // A draw with a RECT aperture is imported as a round-capped stroke of the
    // SMALLER side (G1 debt); on export the ORIGINAL definition must come
    // back — gerbv then paints the exported file exactly like the original.
    const QByteArray src =
        "%FSLAX26Y26*%\n%MOMM*%\nG01*\n%ADD10R,1.5X0.5*%\nD10*\n"
        "X0Y0D02*\nX10000000D01*\nM02*\n";
    const GerberParseResult r = parseGerberData(src);
    REQUIRE(r.ok);
    Document doc;
    importInto(doc, r.file);

    const QByteArray bytes = exportLayer(doc);
    const QString text = QString::fromUtf8(bytes);
    CHECK(text.contains(QStringLiteral("%ADD10R,1.5X0.5*%")));

    // Now EDIT the width: the trace no longer matches the rect aperture and
    // must get a fresh round aperture of the edited width.
    const EntityId id = doc.drawOrder().front();
    {
        TransactionScope tx(doc, QStringLiteral("T"));
        auto* pl = dynamic_cast<PolylineEntity*>(doc.beginModify(id));
        REQUIRE(pl != nullptr);
        pl->setWidth(0.8);
        doc.endModify(id);
        tx.commit();
    }
    const QString text2 = QString::fromUtf8(exportLayer(doc));
    CHECK(text2.contains(QStringLiteral("%ADD10C,0.8*%")));
    CHECK(!text2.contains(QStringLiteral("R,1.5")));
}

TEST_CASE("Gerber writer: insert transforms fold into standard apertures",
          "[gerber][export]")
{
    const GerberFile f = parseFileOrFail(goldenPath("flash_std.gbr"));
    Document doc;
    importInto(doc, f);
    // Flashes in file order: D10 C / D11 R / D12 O / D13 P / D14 C+hole.
    std::vector<EntityId> inserts;
    for (const EntityId id : doc.drawOrder())
        if (dynamic_cast<const InsertEntity*>(doc.entity(id)))
            inserts.push_back(id);
    REQUIRE(inserts.size() == 5);

    const auto modify = [&doc](EntityId id, double rot, double sx, double sy) {
        TransactionScope tx(doc, QStringLiteral("T"));
        auto* ins = dynamic_cast<InsertEntity*>(doc.beginModify(id));
        REQUIRE(ins != nullptr);
        ins->rotation = rot;
        ins->scale = sx;
        ins->scaleY = sy;
        doc.endModify(id);
        tx.commit();
    };

    SECTION("uniform circle scale becomes a scaled C definition") {
        modify(inserts[0], 0.0, 2.0, 0.0);
        GerberExportResult res;
        const QString text = QString::fromUtf8(exportLayer(doc, &res));
        CHECK(text.contains(QStringLiteral("C,3.000248"))); // 1.500124 * 2
        CHECK(res.warnings.isEmpty());
    }
    SECTION("90-degree rect rotation swaps the sides") {
        modify(inserts[1], M_PI_2, 1.0, 0.0);
        const QString text = QString::fromUtf8(exportLayer(doc));
        CHECK(text.contains(QStringLiteral("R,1.016X1.524"))); // was 1.524x1.016
    }
    SECTION("polygon rotation folds into the rotation parameter") {
        modify(inserts[3], M_PI_2, 1.0, 0.0);
        const QString text = QString::fromUtf8(exportLayer(doc));
        CHECK(text.contains(QStringLiteral("P,2.54X6X120"))); // 30 + 90
    }
    SECTION("off-axis rect and non-uniform scale fall back to an outline macro") {
        modify(inserts[1], M_PI / 6.0, 1.0, 0.0); // 30 degrees
        modify(inserts[2], 0.0, 2.0, 1.0);        // anisotropic obround
        GerberExportResult res;
        const QString text = QString::fromUtf8(exportLayer(doc, &res));
        CHECK(text.contains(QStringLiteral("%AMVKOUT1*")));
        CHECK(text.contains(QStringLiteral("%AMVKOUT2*")));
        CHECK(text.contains(QStringLiteral("4,1,"))); // outline primitive
        bool warned = false;
        for (const QString& w : res.warnings)
            warned = warned || w.contains(QStringLiteral("outline macro"));
        CHECK(warned);
        // The fallback must still round-trip geometrically.
        const Document doc2 = reimport(exportLayer(doc));
        const QString diff = compareDocs(doc, doc2, 1e-6, 1e-5);
        INFO(diff.toStdString());
        CHECK(diff.isEmpty());
    }
    SECTION("round-trip of the scaled circle keeps the doubled footprint") {
        modify(inserts[0], 0.0, 2.0, 0.0);
        const Document doc2 = reimport(exportLayer(doc));
        const QString diff = compareDocs(doc, doc2, 1e-6, 1e-5);
        INFO(diff.toStdString());
        CHECK(diff.isEmpty());
    }
}

TEST_CASE("Gerber writer: %AM macros re-emitted verbatim, also after .vkd",
          "[gerber][export]")
{
    const GerberFile f = parseFileOrFail(goldenPath("macro_roundedrect.gbr"));
    Document doc;
    importInto(doc, f);

    const QString text = QString::fromUtf8(exportLayer(doc));
    CHECK(text.contains(QStringLiteral("%AMROUNDEDRECTD15*")));
    // Name kept verbatim, D-code renumbered from 10.
    CHECK(text.contains(QStringLiteral("%ADD10ROUNDEDRECTD15*%")));
    // Rotation of the rect primitives survives verbatim (270 degrees).
    CHECK(text.contains(QStringLiteral(",270*")));

    // The macro body must survive a .vkd save/load cycle (camMeta "macros").
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString vkd = tmp.filePath(QStringLiteral("macro.vkd"));
    QString err;
    REQUIRE(NativeStore::save(doc, vkd, err));
    const auto loaded = NativeStore::load(vkd, err);
    REQUIRE(loaded != nullptr);
    const QString text2 = QString::fromUtf8(exportLayer(*loaded));
    CHECK(text2.contains(QStringLiteral("%AMROUNDEDRECTD15*")));

    const Document doc2 = reimport(text2.toUtf8());
    const QString diff = compareDocs(doc, doc2, 1e-6, 1e-5);
    INFO(diff.toStdString());
    CHECK(diff.isEmpty());
}

TEST_CASE("Gerber writer: errors and empty layers", "[gerber][export]")
{
    Document doc;
    QByteArray bytes;
    const GerberExportResult bad =
        writeGerberLayer(doc, QStringLiteral("Nope"), bytes);
    CHECK(!bad.ok);
    CHECK(bad.error.contains(QStringLiteral("Nope")));

    doc.ensureLayer(QStringLiteral("L"));
    GerberExportResult res;
    const QByteArray out = exportLayer(doc, &res);
    CHECK(res.entities == 0);
    bool warned = false;
    for (const QString& w : res.warnings)
        warned = warned || w.contains(QStringLiteral("no exportable entities"));
    CHECK(warned);
    // Still a valid, parseable file.
    CHECK(parseGerberData(out).ok);
}

// ---------------------------------------------------------------------------
// The G3 truth test: gerbv vs gerbv on the real kits
// ---------------------------------------------------------------------------

TEST_CASE("Gerber writer: gerbv renders exported kit layers like the originals",
          "[gerber][export][kits]")
{
    if (!kitsPresent()) {
        SKIP("real Gerber kits not present on this machine");
    }
    const QString gerbv = QStandardPaths::findExecutable(QStringLiteral("gerbv"));
    if (gerbv.isEmpty()) {
        SKIP("gerbv not installed (sudo apt install gerbv)");
    }
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());

    int tested = 0;
    for (const char* kit : {"S5M0PCBA", "S5M0PCBB"}) {
        const QDir dir(QLatin1String(kKitRoot) + QLatin1Char('/') +
                       QLatin1String(kit));
        const auto entries = dir.entryInfoList(QDir::Files, QDir::Name);
        for (const QFileInfo& fi : entries) {
            const QString ext = fi.suffix().toUpper();
            if (!ext.startsWith(QLatin1Char('G')))
                continue; // Gerber layers only (drills are the Excellon writer's job)
            const GerberParseResult r = parseGerber(fi.absoluteFilePath());
            if (!r.ok || r.file.objects.empty())
                continue; // header-only layers have nothing to compare

            const QString name = QLatin1String(kit) + QLatin1Char('/') + fi.fileName();
            INFO(name.toStdString());

            Document doc;
            importInto(doc, r.file);
            const QString exported =
                tmp.filePath(QLatin1String(kit) + QLatin1Char('-') + fi.fileName() +
                             QStringLiteral(".gbr"));
            const GerberExportResult res =
                exportGerberLayer(doc, QStringLiteral("L"), exported);
            INFO(res.error.toStdString());
            REQUIRE(res.ok);

            // Semantic re-import: same entities, coordinates within 1e-3 mm.
            const GerberParseResult r2 = parseGerber(exported);
            REQUIRE(r2.ok);
            Document doc2;
            importInto(doc2, r2.file);
            const QString diff = compareDocs(doc, doc2, 1e-3, 1e-3);
            INFO(diff.toStdString());
            CHECK(diff.isEmpty());

            // The truth: gerbv renders BOTH files; the ink must match.
            const QString refPng = exported + QStringLiteral(".ref.png");
            const QString gotPng = exported + QStringLiteral(".got.png");
            REQUIRE(renderWithGerbv(gerbv, fi.absoluteFilePath(), refPng));
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
            ++tested;
        }
    }
    // Both kits together expose at least 20 non-empty Gerber layers.
    CHECK(tested >= 20);
}
