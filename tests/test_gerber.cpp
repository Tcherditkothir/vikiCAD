// Gerber RS-274X parser + entity conversion (G1 stage).
//
// Two tiers:
//  - synthetic goldens committed in tests/golden/gerber/, one construct each,
//    with expected values hand-decoded in comments;
//  - the real Altium Designer 18 kits under /home/lex/computer/pcb-ref/
//    (private, machine-local): those tests SKIP when the directory is absent.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <set>

#include "doc/Annotations.h"
#include "doc/Block.h"
#include "doc/Document.h"
#include "doc/EntitiesEx.h"
#include "io/GerberIo.h"
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

GerberFile parseGolden(const char* name)
{
    const GerberParseResult r = parseGerber(goldenPath(name));
    INFO(r.error.toStdString());
    REQUIRE(r.ok);
    return r.file;
}

// Entities of the document in draw order, dynamically typed.
template <typename T>
std::vector<const T*> entitiesOfType(const Document& doc)
{
    std::vector<const T*> out;
    for (const EntityId id : doc.drawOrder())
        if (const auto* e = dynamic_cast<const T*>(doc.entity(id)))
            out.push_back(e);
    return out;
}

double shoelaceArea(const std::vector<Vec2d>& ring)
{
    double a = 0.0;
    for (size_t i = 0; i < ring.size(); ++i) {
        const Vec2d& p = ring[i];
        const Vec2d& q = ring[(i + 1) % ring.size()];
        a += p.cross(q);
    }
    return a / 2.0;
}

} // namespace

// ---------------------------------------------------------------------------
// Synthetic goldens
// ---------------------------------------------------------------------------

TEST_CASE("Gerber: standard aperture flashes (C/R/O/P + hole)", "[gerber]")
{
    // flash_std.gbr, FSLAX25Y25 + MOIN. Hand-decode:
    //   X100000 = 1.00000 in = 25.4 mm (leading zeros omitted, 2.5 => /1e5)
    //   D10 C,0.05906   -> dia 0.05906 in  = 1.500124 mm
    //   D11 R,0.06X0.04 -> 1.524 x 1.016 mm
    //   D12 O,0.08X0.04 -> 2.032 x 1.016 mm
    //   D13 P,0.1X6X30  -> outer dia 2.54 mm, 6 vertices, rot 30 deg
    //   D14 C,0.06X0.02 -> dia 1.524 mm, round hole 0.508 mm
    const GerberFile f = parseGolden("flash_std.gbr");

    CHECK(f.unit == GerberUnit::Inches);
    CHECK(f.format.omitLeading);
    CHECK(f.format.intDigits == 2);
    CHECK(f.format.decDigits == 5);
    CHECK(f.sawM02);
    REQUIRE(f.apertures.size() == 5);

    const GerberAperture& d10 = f.apertures.at(10);
    CHECK(d10.kind == 'C');
    CHECK(d10.params.at(0) == Approx(1.500124).margin(1e-9));
    const GerberAperture& d11 = f.apertures.at(11);
    CHECK(d11.kind == 'R');
    CHECK(d11.params.at(0) == Approx(1.524).margin(1e-9));
    CHECK(d11.params.at(1) == Approx(1.016).margin(1e-9));
    const GerberAperture& d12 = f.apertures.at(12);
    CHECK(d12.kind == 'O');
    CHECK(d12.params.at(0) == Approx(2.032).margin(1e-9));
    const GerberAperture& d13 = f.apertures.at(13);
    CHECK(d13.kind == 'P');
    CHECK(d13.params.at(0) == Approx(2.54).margin(1e-9));
    CHECK(d13.params.at(1) == Approx(6.0));
    CHECK(d13.params.at(2) == Approx(30.0)); // degrees, NOT converted
    const GerberAperture& d14 = f.apertures.at(14);
    CHECK(d14.holeDiameter == Approx(0.508).margin(1e-9));

    REQUIRE(f.objects.size() == 5);
    for (size_t i = 0; i < 5; ++i) {
        CHECK(f.objects[i].kind == GerberObjKind::Flash);
        CHECK(f.objects[i].dark);
        CHECK(f.objects[i].to.x == Approx(25.4 * double(i + 1)).margin(1e-9));
        CHECK(f.objects[i].to.y == Approx(25.4).margin(1e-9));
    }

    Document doc;
    const GerberImportResult res = gerberToDocument(doc, f, QStringLiteral("GTL"));
    INFO(res.error.toStdString());
    REQUIRE(res.ok);
    CHECK(res.flashes == 5);
    CHECK(res.blocks == 5);
    CHECK(res.entities == 5);
    REQUIRE(doc.layerByName(QStringLiteral("GTL")) != nullptr);

    // Rect block: one solid 4-corner ring at +-w/2, +-h/2.
    const BlockDef* rect = doc.blockByName(QStringLiteral("GBR-D11"));
    REQUIRE(rect != nullptr);
    REQUIRE(rect->entities.size() == 1);
    const auto* rectHatch = dynamic_cast<const HatchEntity*>(rect->entities[0].get());
    REQUIRE(rectHatch != nullptr);
    CHECK(rectHatch->pattern == QStringLiteral("SOLID"));
    REQUIRE(rectHatch->rings.size() == 1);
    REQUIRE(rectHatch->rings[0].size() == 4);
    const BBox2d rbox = rectHatch->bounds();
    CHECK(rbox.min.x == Approx(-0.762).margin(1e-9));
    CHECK(rbox.max.y == Approx(0.508).margin(1e-9));

    // Polygon block: 6 vertices, first at 30 degrees.
    const BlockDef* poly = doc.blockByName(QStringLiteral("GBR-D13"));
    REQUIRE(poly != nullptr);
    const auto* polyHatch = dynamic_cast<const HatchEntity*>(poly->entities[0].get());
    REQUIRE(polyHatch != nullptr);
    REQUIRE(polyHatch->rings[0].size() == 6);
    // First vertex: 1.27 mm * (cos 30, sin 30) = (1.0998855.., 0.635)
    CHECK(polyHatch->rings[0][0].x == Approx(1.27 * std::sqrt(3.0) / 2.0).margin(1e-9));
    CHECK(polyHatch->rings[0][0].y == Approx(0.635).margin(1e-9));

    // Circle block: tessellated ring stays within the disc, close to it.
    const BlockDef* circ = doc.blockByName(QStringLiteral("GBR-D10"));
    REQUIRE(circ != nullptr);
    const auto* circHatch = dynamic_cast<const HatchEntity*>(circ->entities[0].get());
    REQUIRE(circHatch != nullptr);
    const BBox2d cbox = circHatch->bounds();
    CHECK(cbox.max.x <= 0.750062 + 1e-9);
    CHECK(cbox.max.x >= 0.750062 - 0.005);

    // Obround block exists; the D14 hole is kept in the table but rendered
    // filled — with a warning.
    CHECK(doc.blockByName(QStringLiteral("GBR-D12")) != nullptr);
    bool holeWarned = false;
    for (const QString& w : res.warnings)
        holeWarned = holeWarned || w.contains(QStringLiteral("D14"));
    CHECK(holeWarned);

    // Inserts land at the flash positions, in order.
    const auto inserts = entitiesOfType<InsertEntity>(doc);
    REQUIRE(inserts.size() == 5);
    CHECK(inserts[0]->blockName == QStringLiteral("GBR-D10"));
    CHECK(inserts[4]->blockName == QStringLiteral("GBR-D14"));
    CHECK(inserts[2]->position.x == Approx(76.2).margin(1e-9));
}

TEST_CASE("Gerber: Altium ROUNDEDRECT macro, rotation around macro origin", "[gerber]")
{
    // macro_roundedrect.gbr carries Altium 18's ROUNDEDRECTD15 macro verbatim:
    // a 23.62 x 35.43 mil rounded-rect pad rotated 270 degrees.
    //   21,1,0.02362,0.03118,0,0,270.0 -> center rect 0.599948 x 0.791972 mm,
    //     centered at origin, rotated 270 CCW => occupies x in +-0.791972/2,
    //     y in +-0.599948/2 ((x,y) -> (y,-x)).
    //   21,1,0.01937,0.03543,0,0,270.0 -> 0.491998 x 0.899922 mm, same rule
    //     => x in +-0.449961, y in +-0.245999.
    //   1,1,0.00425,-0.01559,-0.00969 -> corner circle dia 0.107950 mm at
    //     (-0.395986, -0.246126) mm; NO rotation parameter — Altium already
    //     baked the 270-degree rotation into the circle centers (compare its
    //     D16 macro at rotation 0: centers are (+-0.00969, +-0.01559)).
    // Flash: X393701Y787402 -> (3.93701, 7.87402) in = (100.0000540,
    // 200.0001080) mm.
    // Total pad footprint after rotation: 0.899922 wide x 0.599948 high.
    const GerberFile f = parseGolden("macro_roundedrect.gbr");

    REQUIRE(f.macros.count(QStringLiteral("ROUNDEDRECTD15")) == 1);
    const GerberMacro& m = f.macros.at(QStringLiteral("ROUNDEDRECTD15"));
    REQUIRE(m.prims.size() == 6);
    CHECK(m.prims[0].code == 21);
    CHECK(m.prims[0].params.at(0) == Approx(1.0));            // exposure: count, not mm
    CHECK(m.prims[0].params.at(1) == Approx(0.599948).margin(1e-9)); // 0.02362 in
    CHECK(m.prims[0].params.at(2) == Approx(0.791972).margin(1e-9)); // 0.03118 in
    CHECK(m.prims[0].params.at(5) == Approx(270.0));          // degrees, not scaled
    CHECK(m.prims[2].code == 1);
    CHECK(m.prims[2].params.at(1) == Approx(0.10795).margin(1e-9));
    CHECK(m.prims[2].params.at(2) == Approx(-0.395986).margin(1e-9));
    CHECK(m.prims[2].params.at(3) == Approx(-0.246126).margin(1e-9));
    REQUIRE(f.apertures.at(15).kind == 'M');
    CHECK(f.apertures.at(15).macroName == QStringLiteral("ROUNDEDRECTD15"));

    Document doc;
    const GerberImportResult res = gerberToDocument(doc, f, QStringLiteral("GTP"));
    REQUIRE(res.ok);
    CHECK(res.flashes == 1);
    CHECK(res.blocks == 1);

    const BlockDef* def = doc.blockByName(QStringLiteral("GBR-D15"));
    REQUIRE(def != nullptr);
    REQUIRE(def->entities.size() == 1);
    const auto* hatch = dynamic_cast<const HatchEntity*>(def->entities[0].get());
    REQUIRE(hatch != nullptr);
    REQUIRE(hatch->rings.size() == 6); // 2 rects + 4 corner circles

    // Rotated footprint check (hand-computed above): x half-width 0.449961 mm
    // (from the second rect), y half-height 0.299974 mm (first rect). Circle
    // rings are tessellated inside their exact discs, tolerance 0.001 mm.
    const BBox2d box = hatch->bounds();
    CHECK(box.max.x == Approx(0.449961).margin(1e-6));
    CHECK(box.min.x == Approx(-0.449961).margin(1e-6));
    CHECK(box.max.y == Approx(0.299974).margin(1e-6));
    CHECK(box.min.y == Approx(-0.299974).margin(1e-6));

    // First rect ring, rotated 270 CCW around the origin: corner
    // (0.299974, 0.395986) -> (0.395986, -0.299974).
    bool foundCorner = false;
    for (const Vec2d& p : hatch->rings[0])
        if (nearEqual(p, Vec2d{0.395986, -0.299974}, 1e-6))
            foundCorner = true;
    CHECK(foundCorner);

    const auto inserts = entitiesOfType<InsertEntity>(doc);
    REQUIRE(inserts.size() == 1);
    CHECK(inserts[0]->position.x == Approx(100.000054).margin(1e-6));
    CHECK(inserts[0]->position.y == Approx(200.000108).margin(1e-6));
}

TEST_CASE("Gerber: round draws, modal coordinates, LPC marks entities", "[gerber]")
{
    // draw_round.gbr: D10 = C,0.01 (0.254 mm), D11 = C,0.02 (0.508 mm).
    // Path 1 (dark):  (0,0) -> X100000 (25.4,0) -> Y100000 (25.4,25.4);
    // the Y-only word keeps X = 25.4 (modal coordinates).
    // Path 2 (clear, after %LPC*%): (50.8,50.8) -> (76.2,50.8).
    const GerberFile f = parseGolden("draw_round.gbr");
    REQUIRE(f.objects.size() == 3);
    CHECK(f.objects[0].dark);
    CHECK(f.objects[1].dark);
    CHECK_FALSE(f.objects[2].dark);
    CHECK(f.objects[1].from.x == Approx(25.4));
    CHECK(f.objects[1].to.y == Approx(25.4));

    Document doc;
    const GerberImportResult res = gerberToDocument(doc, f, QStringLiteral("GTO"));
    REQUIRE(res.ok);
    CHECK(res.draws == 3);
    CHECK(res.entities == 2); // two continuous strokes coalesce into one pline

    const auto plines = entitiesOfType<PolylineEntity>(doc);
    REQUIRE(plines.size() == 2);
    REQUIRE(plines[0]->vertices().size() == 3);
    CHECK(plines[0]->width() == Approx(0.254).margin(1e-9));
    CHECK(plines[0]->vertices()[1].pos.x == Approx(25.4));
    CHECK(plines[0]->vertices()[2].pos.y == Approx(25.4));
    // G2 inspection: the stroking aperture rides on the entity ("dcode");
    // dark entities still carry NO gpol marker.
    CHECK(plines[0]->extra().value(QLatin1String("dcode")).toInt() == 10);
    CHECK_FALSE(plines[0]->extra().contains(QLatin1String("gpol")));
    REQUIRE(plines[1]->vertices().size() == 2);
    CHECK(plines[1]->width() == Approx(0.508).margin(1e-9));
    CHECK(plines[1]->extra().value(QLatin1String("dcode")).toInt() == 11);
    CHECK(plines[1]->extra().value(QLatin1String("gpol")).toString()
          == QStringLiteral("C"));

    // Wide stroke inflates bounds by width/2 and reaches the primitives.
    const BBox2d b = plines[1]->bounds();
    CHECK(b.min.y == Approx(50.8 - 0.254).margin(1e-9));
    RenderContext ctx;
    PrimitiveList list;
    plines[1]->buildPrimitives(ctx, list);
    REQUIRE(list.strokes.size() == 1);
    CHECK(list.strokes[0].width == Approx(0.508).margin(1e-9));
}

TEST_CASE("Gerber: G75 multi-quadrant arcs to bulge polylines", "[gerber]")
{
    // arc_g75.gbr, hand-computed:
    // Arc 1: D02 to (1 in, 0) = (25.4, 0). G03 (CCW),
    //   X0Y-100000I-100000J0 -> end (0, -1 in) = (0, -25.4),
    //   center = start + (I, J) = (25.4, 0) + (-25.4, 0) = (0, 0).
    //   Start angle 0, end angle 270 deg, CCW sweep = 270 deg — it crosses
    //   quadrants I->II->III (multi-quadrant, only valid under G75).
    //   bulge = tan(270 deg / 4) = tan(67.5 deg) = 2.4142135...
    // Arc 2: D02 to (3 in, 0) = (76.2, 0). G02 (CW), end == start ->
    //   FULL CIRCLE, center = (76.2, 0) + (25.4, 0) = (101.6, 0), r = 25.4.
    //   Converted as two CW half circles: bulges -1, -1, via the opposite
    //   point 2*center - start = (127, 0).
    const GerberFile f = parseGolden("arc_g75.gbr");
    REQUIRE(f.objects.size() == 2);
    CHECK(f.objects[0].kind == GerberObjKind::Arc);
    CHECK_FALSE(f.objects[0].cw);
    CHECK_FALSE(f.objects[0].fullCircle);
    CHECK(f.objects[0].center.x == Approx(0.0).margin(1e-9));
    CHECK(f.objects[1].fullCircle);
    CHECK(f.objects[1].cw);
    CHECK(f.objects[1].center.x == Approx(101.6).margin(1e-9));

    Document doc;
    const GerberImportResult res = gerberToDocument(doc, f, QStringLiteral("GM1"));
    REQUIRE(res.ok);
    CHECK(res.arcs == 2);

    const auto plines = entitiesOfType<PolylineEntity>(doc);
    REQUIRE(plines.size() == 2);
    REQUIRE(plines[0]->vertices().size() == 2);
    CHECK(plines[0]->vertices()[0].pos.x == Approx(25.4));
    CHECK(plines[0]->vertices()[0].bulge == Approx(std::tan(3.0 * M_PI / 8.0)).margin(1e-9));
    CHECK(plines[0]->vertices()[1].pos.y == Approx(-25.4));

    REQUIRE(plines[1]->vertices().size() == 3);
    CHECK(plines[1]->vertices()[0].pos.x == Approx(76.2));
    CHECK(plines[1]->vertices()[0].bulge == Approx(-1.0).margin(1e-9));
    CHECK(plines[1]->vertices()[1].pos.x == Approx(127.0));
    CHECK(plines[1]->vertices()[1].bulge == Approx(-1.0).margin(1e-9));
    CHECK(plines[1]->vertices()[2].pos.x == Approx(76.2));
}

TEST_CASE("Gerber: G36/G37 regions to solid hatches", "[gerber]")
{
    // region.gbr:
    // Region 1: square (0,0) (25.4,0) (25.4,25.4) (0,25.4), closed by a Y0
    //   draw back to the start, then the Altium-style bare D02* terminator.
    // Region 2: line (50.8,0) -> (76.2,0), then G03 CCW arc back to (50.8,0)
    //   with I-50000 -> center (76.2,0) + (-12.7,0) = (63.5, 0), r = 12.7:
    //   the top half-circle. Enclosed area = pi * r^2 / 2 = 253.3539 mm^2.
    const GerberFile f = parseGolden("region.gbr");
    REQUIRE(f.objects.size() == 2);
    REQUIRE(f.objects[0].kind == GerberObjKind::Region);
    REQUIRE(f.objects[0].contours.size() == 1);
    CHECK(f.objects[0].contours[0].segs.size() == 4);
    REQUIRE(f.objects[1].contours.size() == 1);
    CHECK(f.objects[1].contours[0].segs[1].isArc);
    CHECK(f.objects[1].contours[0].segs[1].center.x == Approx(63.5).margin(1e-9));

    Document doc;
    const GerberImportResult res = gerberToDocument(doc, f, QStringLiteral("GKO"));
    REQUIRE(res.ok);
    CHECK(res.regions == 2);

    const auto hatches = entitiesOfType<HatchEntity>(doc);
    REQUIRE(hatches.size() == 2);
    CHECK(hatches[0]->pattern == QStringLiteral("SOLID"));
    REQUIRE(hatches[0]->rings.size() == 1);
    CHECK(hatches[0]->rings[0].size() == 4); // exact square, closing point dropped

    REQUIRE(hatches[1]->rings.size() == 1);
    const auto& dRing = hatches[1]->rings[0];
    CHECK(dRing.size() > 20); // arc tessellated (0.001 mm sagitta)
    const BBox2d b = hatches[1]->bounds();
    CHECK(b.min.x == Approx(50.8).margin(1e-6));
    CHECK(b.max.x == Approx(76.2).margin(1e-6));
    CHECK(b.min.y == Approx(0.0).margin(1e-6));
    CHECK(b.max.y == Approx(12.7).margin(0.001)); // chord flattening only shrinks
    CHECK(shoelaceArea(dRing) == Approx(M_PI * 12.7 * 12.7 / 2.0).margin(0.5));
}

TEST_CASE("Gerber: LPC/LPD order preserved, gpol + width survive .vkd", "[gerber]")
{
    // polarity_order.gbr paints: dark flash (D10, dia 1.27 mm at (12.7,12.7)),
    // then a CLEAR square region punched over it, then a dark trace (D11,
    // 0.254 mm) across. Erase semantics = paint order; the document must keep
    // insert -> hatch -> polyline in draw order with gpol only on the hatch.
    const GerberFile f = parseGolden("polarity_order.gbr");
    REQUIRE(f.objects.size() == 3);
    CHECK(f.objects[0].dark);
    CHECK_FALSE(f.objects[1].dark);
    CHECK(f.objects[2].dark);

    Document doc;
    const GerberImportResult res = gerberToDocument(doc, f, QStringLiteral("GTL"));
    REQUIRE(res.ok);
    REQUIRE(res.entities == 3);

    auto checkDoc = [](const Document& d) {
        REQUIRE(d.drawOrder().size() == 3);
        const Entity* e0 = d.entity(d.drawOrder()[0]);
        const Entity* e1 = d.entity(d.drawOrder()[1]);
        const Entity* e2 = d.entity(d.drawOrder()[2]);
        REQUIRE(dynamic_cast<const InsertEntity*>(e0) != nullptr);
        REQUIRE(dynamic_cast<const HatchEntity*>(e1) != nullptr);
        const auto* pl = dynamic_cast<const PolylineEntity*>(e2);
        REQUIRE(pl != nullptr);
        // G2 inspection: aperture-painted entities carry their D-code; the
        // region (G36/G37, aperture-less) carries gpol only.
        CHECK(e0->extra().value(QLatin1String("dcode")).toInt() == 10);
        CHECK_FALSE(e0->extra().contains(QLatin1String("gpol")));
        CHECK(e1->extra().value(QLatin1String("gpol")).toString()
              == QStringLiteral("C"));
        CHECK_FALSE(e1->extra().contains(QLatin1String("dcode")));
        CHECK(e2->extra().value(QLatin1String("dcode")).toInt() == 11);
        CHECK_FALSE(e2->extra().contains(QLatin1String("gpol")));
        CHECK(pl->width() == Approx(0.254).margin(1e-9));
    };
    checkDoc(doc);

    // Round-trip through the native store: order, gpol and width survive.
    QTemporaryDir tmp;
    const QString path = tmp.filePath(QStringLiteral("gerber.vkd"));
    QString error;
    REQUIRE(NativeStore::save(doc, path, error));
    const auto loaded = NativeStore::load(path, error);
    INFO(error.toStdString());
    REQUIRE(loaded != nullptr);
    checkDoc(*loaded);

    // The whole import is ONE transaction: a single undo clears it.
    CHECK(doc.undo() == QStringLiteral("GERBERIMPORT"));
    CHECK(doc.entityCount() == 0);
    doc.redo();
    checkDoc(doc);
}

TEST_CASE("Gerber: zero suppression at both extremes", "[gerber]")
{
    // zeros_leading.gbr (FSLA = leading zeros omitted, 2.5 inch):
    //   X1       -> 0.00001 in  = 0.000254 mm (all leading zeros dropped)
    //   Y-1      -> -0.000254 mm
    //   X1000000 -> 10.00000 in = 254 mm (full 7 digits, nothing dropped)
    //   Y0       -> 0
    {
        const GerberFile f = parseGolden("zeros_leading.gbr");
        REQUIRE(f.objects.size() == 1);
        const GerberObject& o = f.objects[0];
        CHECK(o.from.x == Approx(0.000254).margin(1e-12));
        CHECK(o.from.y == Approx(-0.000254).margin(1e-12));
        CHECK(o.to.x == Approx(254.0).margin(1e-9));
        CHECK(o.to.y == Approx(0.0).margin(1e-12));
    }
    // zeros_trailing.gbr (FST = TRAILING zeros omitted: pad right to 7):
    //   X1     -> "1000000" -> 10.00000 in = 254 mm
    //   Y-1    -> -254 mm
    //   X94488 -> "9448800" -> 94.48800 in = 2399.9952 mm
    //   Y2     -> "2000000" -> 20 in = 508 mm
    {
        const GerberFile f = parseGolden("zeros_trailing.gbr");
        REQUIRE(f.objects.size() == 1);
        const GerberObject& o = f.objects[0];
        CHECK_FALSE(f.format.omitLeading);
        CHECK(o.from.x == Approx(254.0).margin(1e-9));
        CHECK(o.from.y == Approx(-254.0).margin(1e-9));
        CHECK(o.to.x == Approx(2399.9952).margin(1e-9));
        CHECK(o.to.y == Approx(508.0).margin(1e-9));
    }
}

TEST_CASE("Gerber: metric file needs no conversion", "[gerber]")
{
    // mm_file.gbr: FSLAX43Y43 + MOMM. X10000 -> 10.000 mm directly.
    const GerberFile f = parseGolden("mm_file.gbr");
    CHECK(f.unit == GerberUnit::Millimeters);
    REQUIRE(f.objects.size() == 1);
    CHECK(f.objects[0].from.x == Approx(10.0).margin(1e-12));
    CHECK(f.objects[0].from.y == Approx(20.0).margin(1e-12));
    CHECK(f.objects[0].to.x == Approx(30.0).margin(1e-12));
    CHECK(f.apertures.at(10).params[0] == Approx(0.25).margin(1e-12));

    Document doc;
    const GerberImportResult res = gerberToDocument(doc, f, QStringLiteral("L1"));
    REQUIRE(res.ok);
    const auto plines = entitiesOfType<PolylineEntity>(doc);
    REQUIRE(plines.size() == 1);
    CHECK(plines[0]->width() == Approx(0.25).margin(1e-12));
}

TEST_CASE("Gerber: G74 single-quadrant arcs (unsigned I/J)", "[gerber]")
{
    // g74_single.gbr, hand-computed. Under G74 the I/J offsets are UNSIGNED;
    // the center is the sign choice giving an arc of at most 90 degrees.
    // Arc 1: start (1 in,0), G03 CCW to (0,1 in), I100000 (1 in) J0.
    //   Candidates: (2 in,0) [start+(+1,0)] or (0,0) [start+(-1,0)].
    //   (0,0): r1 = r2 = 1 in, CCW 0 -> 90 deg = 90 deg <= 90  -> VALID.
    //   (2 in,0): radii 1 vs sqrt(5), CCW sweep 333 deg       -> invalid.
    //   => center (0,0), sweep +90 deg, bulge = tan(22.5) = 0.4142135...
    // Arc 2: start (0,-1 in), G02 CW to (-1 in,0), I0 J100000.
    //   (0,0): r1 = r2 = 1, CW sweep from -90 deg to 180 deg = 90 deg -> VALID
    //   (0,-2 in): radii 1 vs sqrt(5), CW sweep 333 deg -> invalid.
    //   => center (0,0), sweep -90 deg, bulge = -0.4142135...
    const GerberFile f = parseGolden("g74_single.gbr");
    REQUIRE(f.objects.size() == 2);
    CHECK(f.objects[0].center.x == Approx(0.0).margin(1e-9));
    CHECK(f.objects[0].center.y == Approx(0.0).margin(1e-9));
    CHECK(f.objects[1].center.x == Approx(0.0).margin(1e-9));
    CHECK(f.objects[1].center.y == Approx(0.0).margin(1e-9));

    Document doc;
    REQUIRE(gerberToDocument(doc, f, QStringLiteral("GM1")).ok);
    const auto plines = entitiesOfType<PolylineEntity>(doc);
    REQUIRE(plines.size() == 2);
    CHECK(plines[0]->vertices()[0].bulge == Approx(std::tan(M_PI / 8.0)).margin(1e-9));
    CHECK(plines[0]->vertices()[1].pos.y == Approx(25.4).margin(1e-9));
    CHECK(plines[1]->vertices()[0].bulge == Approx(-std::tan(M_PI / 8.0)).margin(1e-9));
    CHECK(plines[1]->vertices()[1].pos.x == Approx(-25.4).margin(1e-9));

    // A G74 full circle (identical endpoints) is undefined — explicit error,
    // never a silent wrong arc.
    const QByteArray bad =
        "%FSLAX25Y25*%\n%MOIN*%\nG74*\n%ADD10C,0.01*%\nD10*\n"
        "X100000Y0D02*\nG03*\nX100000Y0I100000J0D01*\nM02*\n";
    const GerberParseResult r = parseGerberData(bad);
    REQUIRE_FALSE(r.ok);
    CHECK(r.error.contains(QStringLiteral("G74")));
    CHECK(r.error.contains(QStringLiteral("line 8")));
}

TEST_CASE("Gerber: X2 attributes in both forms", "[gerber]")
{
    // attrs_x2.gbr has the Altium comment form on line 1
    // (G04 #@! TF.GenerationSoftware,...) AND naked %TF...*% statements.
    const GerberFile f = parseGolden("attrs_x2.gbr");
    CHECK(f.fileAttributes.value(QStringLiteral(".GenerationSoftware"))
          == QStringLiteral("Altium Limited,Altium Designer,18.0.9 (584)"));
    CHECK(f.fileAttributes.value(QStringLiteral(".FileFunction"))
          == QStringLiteral("Copper,L1,Top"));
    CHECK(f.fileAttributes.value(QStringLiteral(".Part")) == QStringLiteral("Single"));
}

TEST_CASE("Gerber: errors carry line numbers", "[gerber]")
{
    auto expectError = [](const char* data, const char* needle, int line) {
        const GerberParseResult r = parseGerberData(QByteArray(data));
        INFO(data);
        REQUIRE_FALSE(r.ok);
        INFO(r.error.toStdString());
        CHECK(r.error.contains(QLatin1String(needle)));
        if (line > 0)
            CHECK(r.error.contains(QStringLiteral("line %1:").arg(line)));
    };

    // Coordinate before the %FS format statement.
    expectError("%MOIN*%\nG01*\nX100Y100D02*\nM02*\n", "%FS", 3);
    // Undefined aperture selected.
    expectError("%FSLAX25Y25*%\n%MOIN*%\nG01*\nD99*\nM02*\n", "D99", 4);
    // Arc before any quadrant mode.
    expectError("%FSLAX25Y25*%\n%MOIN*%\n%ADD10C,0.01*%\nD10*\nX0Y0D02*\nG03*\n"
                "X100Y100I50J50D01*\nM02*\n",
                "G74/G75", 7);
    // Draw with no aperture selected.
    expectError("%FSLAX25Y25*%\n%MOIN*%\nG01*\nX0Y0D02*\nX100D01*\nM02*\n",
                "no aperture", 5);
    // Non-identity step & repeat.
    expectError("%FSLAX25Y25*%\n%MOIN*%\n%SRX2Y3I1.5J2.2*%\nM02*\n",
                "step & repeat", 3);
    // Incremental coordinates.
    expectError("%FSLAX25Y25*%\n%MOIN*%\nG91*\nM02*\n", "G91", 3);
    // Negative image polarity.
    expectError("%FSLAX25Y25*%\n%MOIN*%\n%IPNEG*%\nM02*\n", "polarity", 3);
    // Macro variables are out of scope — explicit refusal.
    expectError("%FSLAX25Y25*%\n%MOIN*%\n%AMVAR*\n1,1,$1,0,0*\n%\nM02*\n",
                "variables", 3);
    // Unterminated extended command.
    expectError("%FSLAX25Y25*\nM02*\n", "unterminated", 1);
    // Region never closed.
    expectError("%FSLAX25Y25*%\n%MOIN*%\nG01*\nG36*\nX0Y0D02*\nX100D01*\nM02*\n",
                "G36", 4);
    // D00 does not exist in RS-274X — reject, never draw silently as a D01.
    expectError("%FSLAX25Y25*%\n%MOIN*%\nG01*\n%ADD10C,0.01*%\nD10*\nX0Y0D02*\n"
                "X100000Y0D00*\nM02*\n",
                "invalid operation code D0", 7);
    // No unit anywhere.
    expectError("%FSLAX25Y25*%\nG01*\n%ADD10C,0.01*%\nD10*\nX0Y0D02*\nX100D01*\nM02*\n",
                "unit", 0);
}

// ---------------------------------------------------------------------------
// Real Altium kits (skip when /home/lex/computer/pcb-ref is absent)
// ---------------------------------------------------------------------------

namespace {

const char* kKitAExts[] = {"GTL", "GBL", "GTS", "GBS", "GTO", "GBO", "GTP",
                           "GBP", "GKO", "GM1", "GM13", "GM15", "GPT", "GPB"};
const char* kKitBExts[] = {"GTL", "GBL", "GTS", "GBS", "GTO", "GBO", "GTP", "GBP",
                           "GKO", "GM1", "GM4", "GM13", "GM15", "GM16", "GPT", "GPB"};

QString kitFile(const char* kit, const char* prefix, const char* ext)
{
    return QStringLiteral("%1/%2/%3.%4")
        .arg(QLatin1String(kKitRoot), QLatin1String(kit), QLatin1String(prefix),
             QLatin1String(ext));
}

// "Used DCodes" per generated file, parsed from the Altium .REP report —
// an independent ground truth for the aperture usage of every layer.
QMap<QString, std::set<int>> parseRepDcodes(const QString& path)
{
    QMap<QString, std::set<int>> out;
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    QString current;
    bool collecting = false;
    while (!f.atEnd()) {
        const QString line = QString::fromLatin1(f.readLine()).trimmed();
        if (line.startsWith(QStringLiteral("File :"))) {
            current = line.mid(6).trimmed().section(QLatin1Char('.'), -1);
            out[current]; // ensure the entry exists even with no D-codes
            collecting = false;
            continue;
        }
        if (line.startsWith(QStringLiteral("Used DCodes"))) {
            collecting = true;
            continue;
        }
        if (collecting) {
            if (line.startsWith(QLatin1Char('D')) && line.size() > 1) {
                bool ok = false;
                const int d = line.mid(1).toInt(&ok);
                if (ok)
                    out[current].insert(d);
            } else if (line.startsWith(QLatin1Char('*'))) {
                collecting = false;
            }
        }
    }
    return out;
}

std::set<int> objectDcodes(const GerberFile& f)
{
    std::set<int> used;
    for (const GerberObject& o : f.objects)
        if (o.kind != GerberObjKind::Region)
            used.insert(o.dcode);
    return used;
}

BBox2d gerberBounds(const GerberFile& f)
{
    BBox2d box;
    for (const GerberObject& o : f.objects) {
        if (o.kind == GerberObjKind::Region) {
            for (const GerberContour& c : o.contours) {
                box.expand(c.start);
                for (const GerberContourSeg& s : c.segs)
                    box.expand(s.to);
            }
            continue;
        }
        box.expand(o.to);
        if (o.kind != GerberObjKind::Flash)
            box.expand(o.from);
    }
    return box;
}

} // namespace

TEST_CASE("Gerber kits: every layer parses; D-codes match the .REP report", "[gerber][kits]")
{
    if (!kitsPresent())
        SKIP("real Gerber kits not present on this machine");

    struct Kit {
        const char* dir;
        const char* prefix;
        const char** exts;
        size_t extCount;
    };
    const Kit kits[] = {
        {"S5M0PCBA", "S5M0PCBA1", kKitAExts, std::size(kKitAExts)},
        {"S5M0PCBB", "S5M0PCBB1", kKitBExts, std::size(kKitBExts)},
    };

    for (const Kit& kit : kits) {
        const auto rep = parseRepDcodes(
            kitFile(kit.dir, kit.prefix, "REP"));
        for (size_t i = 0; i < kit.extCount; ++i) {
            const QString path = kitFile(kit.dir, kit.prefix, kit.exts[i]);
            INFO(path.toStdString());
            const GerberParseResult r = parseGerber(path);
            INFO(r.error.toStdString());
            REQUIRE(r.ok);
            CHECK(r.file.sawM02);
            CHECK(r.file.unit == GerberUnit::Inches);
            CHECK(r.file.format.omitLeading);
            CHECK(r.file.format.intDigits == 2);
            CHECK(r.file.format.decDigits == 5);
            INFO(r.file.warnings.join(QStringLiteral(" | ")).toStdString());
            CHECK(r.file.warnings.isEmpty());

            // Every D-code used by an object must be listed in the report,
            // and every report D-code must be defined in the file.
            const std::set<int> used = objectDcodes(r.file);
            const QString ext = QLatin1String(kit.exts[i]);
            REQUIRE(rep.contains(ext));
            const std::set<int>& reported = rep[ext];
            for (const int d : used) {
                INFO("object D-code " << d << " not in .REP for " << ext.toStdString());
                CHECK(reported.count(d) == 1);
            }
            for (const int d : reported) {
                INFO(".REP D-code " << d << " not defined in " << ext.toStdString());
                CHECK(r.file.apertures.count(d) == 1);
            }
        }

        // Exact aperture-usage equality, hand-checked against the .REP:
        //   S5M0PCBA1.GBO "Used DCodes : D10 D11 D14" — all three stroke.
        if (QLatin1String(kit.dir) == QLatin1String("S5M0PCBA")) {
            const GerberParseResult r =
                parseGerber(kitFile(kit.dir, kit.prefix, "GBO"));
            REQUIRE(r.ok);
            CHECK(objectDcodes(r.file) == std::set<int>{10, 11, 14});
        }
    }
}

TEST_CASE("Gerber kits: object-count snapshots on curated layers", "[gerber][kits]")
{
    if (!kitsPresent())
        SKIP("real Gerber kits not present on this machine");

    // Counts snapshot verified against an independent region-aware awk pass
    // over the raw files, after manually decoding 3 raw lines per file
    // (FSLAX25Y25 + MOIN everywhere: value = int / 1e5 inches, x25.4 -> mm).
    //
    // S5M0PCBA1.GTL   l.72 "X353844Y24109D02*"  -> move (89.876376, 6.123686) mm
    //                 l.73 "X350006D01*"        -> draw to (88.901524, 6.123686), Y modal
    //                 l.74 "Y30715D01*"         -> draw to (88.901524, 7.801610), X modal
    // S5M0PCBA1.GTO   l.16 "X215945Y48819D02*"  -> move (54.850030, 12.400026)
    //                 l.18 "X215945Y48819I-394J0D01*" after G03 -> end == start
    //                      => FULL CIRCLE, center (54.850030-0.100076, 12.400026)
    //                 l.20 "X231496Y61811D02*"  -> move (58.799984, 15.699994)
    // S5M0PCBA1.GM13  l.15 "X182677Y85829D02*"  -> region contour start
    //                      (46.399958, 21.800566)
    //                 l.16 "X181301D01*"        -> edge to (46.050454, 21.800566)
    //                 l.17 "Y87205D01*"         -> edge to (46.050454, 22.150070)
    // S5M0PCBA1.GTS   l.61 "X190551Y80905D02*"  -> move (48.399954, 20.549870)
    //                 l.62 "D03*"               -> BARE flash at current point
    //                 l.63 "Y86811D02*"         -> move (48.399954, 22.049994)
    // S5M0PCBA1.GKO   l.6 "%FSLAX25Y25*%", l.7 "%MOIN*%", l.10 "M02*"
    //                      => header only, ZERO objects (the real outline
    //                      lives on GM1/GM13 — tolerant contour heuristic).
    // S5M0PCBA1.GBO   l.14 "X81890Y-42323D02*"  -> move (20.800060, -10.750042);
    //                      negative Y decodes through the sign prefix
    //                 l.16 "X81890Y-42323I-394J0D01*" after G03 -> full circle
    //                 l.18 "X247244D02*"        -> move (62.799976, -10.750042)
    // S5M0PCBB1.GTL   l.67 "X247735Y208174D02*" -> move (62.924690, 52.876196)
    //                 l.68 "X247735Y153051D01*" -> draw to (62.924690, 38.874954)
    //                 l.69 "X247738Y153043D01*" -> draw to (62.925452, 38.872922)
    // S5M0PCBB1.GBL   l.36-38: same three words as GTL l.67-69 (same via path)
    // S5M0PCBB1.GKO   l.11 "X113819Y179961D02*" -> region start (28.910026, 45.710094)
    //                 l.12 "X44173D01*"         -> edge to (11.219942, 45.710094)
    //                 l.13 "Y205512D01*"        -> edge to (11.219942, 52.200048)
    // S5M0PCBB1.GM1   l.12 "X306496Y167102D02*" -> move (77.849984, 42.443908)
    //                 l.14 "X313976Y167102I3740J0D01*" after G03 -> CCW arc to
    //                      (79.749904, 42.443908), center (78.799944, 42.443908):
    //                      half circle, r = 0.949960 mm
    //                 l.16 "X248031Y153323D02*" -> move (62.999874, 38.944042)
    // S5M0PCBB1.GBO   l.17 "X135236Y78740D02*"  -> move (34.349944, 19.999960)
    //                 l.19 "X135236Y78740I-394J0D01*" -> full circle r 0.100076
    //                 l.21 "X259252Y118110D02*" -> move (65.850008, 29.999940)
    // S5M0PCBB1.GM13  l.14 "X183574Y108909D02*" -> region start (46.627796, 27.662886)
    //                 l.15 "X182174D01*"        -> edge to (46.272196, 27.662886)
    //                 l.16 "Y114009D01*"        -> edge to (46.272196, 28.958286)
    struct Snapshot {
        const char* kit;
        const char* prefix;
        const char* ext;
        int draws, arcs, flashes, regions;
    };
    const Snapshot snaps[] = {
        {"S5M0PCBA", "S5M0PCBA1", "GTL", 239, 0, 297, 174},
        {"S5M0PCBA", "S5M0PCBA1", "GTO", 1029, 7, 0, 0},
        {"S5M0PCBA", "S5M0PCBA1", "GM13", 158, 11, 0, 1},
        {"S5M0PCBA", "S5M0PCBA1", "GTS", 0, 0, 184, 0},
        {"S5M0PCBA", "S5M0PCBA1", "GKO", 0, 0, 0, 0},
        {"S5M0PCBA", "S5M0PCBA1", "GBO", 100, 4, 0, 0},
        {"S5M0PCBB", "S5M0PCBB1", "GTL", 172, 4, 418, 132},
        {"S5M0PCBB", "S5M0PCBB1", "GBL", 225, 4, 413, 112},
        {"S5M0PCBB", "S5M0PCBB1", "GKO", 0, 0, 0, 1},
        {"S5M0PCBB", "S5M0PCBB1", "GM1", 18, 2, 0, 0},
        {"S5M0PCBB", "S5M0PCBB1", "GBO", 564, 11, 0, 0},
        {"S5M0PCBB", "S5M0PCBB1", "GM13", 130, 15, 0, 24},
    };
    for (const Snapshot& s : snaps) {
        const GerberParseResult r = parseGerber(kitFile(s.kit, s.prefix, s.ext));
        INFO(s.kit << "." << s.ext);
        REQUIRE(r.ok);
        int draws = 0, arcs = 0, flashes = 0, regions = 0;
        for (const GerberObject& o : r.file.objects) {
            switch (o.kind) {
            case GerberObjKind::Draw: ++draws; break;
            case GerberObjKind::Arc: ++arcs; break;
            case GerberObjKind::Flash: ++flashes; break;
            case GerberObjKind::Region: ++regions; break;
            }
        }
        CHECK(draws == s.draws);
        CHECK(arcs == s.arcs);
        CHECK(flashes == s.flashes);
        CHECK(regions == s.regions);
    }
}

TEST_CASE("Gerber kits: plausible board extents and full conversion", "[gerber][kits]")
{
    if (!kitsPresent())
        SKIP("real Gerber kits not present on this machine");

    // Copper layers of both boards must land in credible PCB dimensions.
    for (const char* kit : {"S5M0PCBA", "S5M0PCBB"}) {
        const QString prefix = QLatin1String(kit) + QLatin1String("1");
        const GerberParseResult r =
            parseGerber(kitFile(kit, prefix.toLatin1().constData(), "GTL"));
        REQUIRE(r.ok);
        const BBox2d box = gerberBounds(r.file);
        REQUIRE(box.isValid());
        INFO(kit << " GTL bbox " << box.width() << " x " << box.height() << " mm");
        CHECK(box.width() > 30.0);
        CHECK(box.width() < 200.0);
        CHECK(box.height() > 30.0);
        CHECK(box.height() < 200.0);
    }

    // Full conversion of the densest layer: counts flow through, entity
    // stream is non-trivial, aperture blocks materialize.
    const GerberParseResult r = parseGerber(kitFile("S5M0PCBA", "S5M0PCBA1", "GTL"));
    REQUIRE(r.ok);
    Document doc;
    const GerberImportResult res = gerberToDocument(doc, r.file, QStringLiteral("GTL"));
    INFO(res.error.toStdString());
    REQUIRE(res.ok);
    CHECK(res.draws == 239);
    CHECK(res.arcs == 0);
    CHECK(res.flashes == 297);
    CHECK(res.regions == 174);
    CHECK(res.blocks == 26);   // distinct D-codes flashed on this layer
    CHECK(doc.entityCount() > 400);
    CHECK(doc.blocks().size() == 26);
}
