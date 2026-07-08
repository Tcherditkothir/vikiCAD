#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <QFile>
#include <QTemporaryDir>

#include "doc/Annotations.h"
#include "doc/Entities.h"
#include "doc/EntitiesEx.h"
#include "io/DxfImporter.h"

using namespace viki;
using Catch::Approx;

namespace {

// Minimal hand-written R12-style ASCII DXF containing a layer, a line,
// a circle, an arc and an LWPOLYLINE (2000-style section for the latter).
const char* kFixture = R"(0
SECTION
2
HEADER
9
$INSUNITS
70
4
0
ENDSEC
0
SECTION
2
TABLES
0
TABLE
2
LAYER
70
1
0
LAYER
2
walls
70
0
62
1
6
CONTINUOUS
0
ENDTAB
0
ENDSEC
0
SECTION
2
ENTITIES
0
LINE
8
walls
10
0.0
20
0.0
11
100.0
21
50.0
0
CIRCLE
8
0
10
10.0
20
20.0
40
5.0
0
ARC
8
0
10
0.0
20
0.0
40
10.0
50
0.0
51
90.0
0
LWPOLYLINE
8
walls
90
3
70
1
10
0.0
20
0.0
10
50.0
20
0.0
42
1.0
10
50.0
20
50.0
0
ENDSEC
0
EOF
)";

} // namespace

TEST_CASE("DXF import: layers, entities, colors, arc angles", "[dxf]")
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("fixture.dxf"));
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(kFixture);
    }

    const DxfImportResult r = importDxf(path);
    REQUIRE(r.ok);
    REQUIRE(r.imported == 4);
    REQUIRE(r.document);

    Document& doc = *r.document;
    REQUIRE(doc.entityCount() == 4);

    // Layer "walls" exists, ACI 1 = red.
    Layer* walls = doc.layerByName(QStringLiteral("walls"));
    REQUIRE(walls);
    REQUIRE(walls->rgb == 0xFF0000);

    // Line on walls layer.
    const auto* line = dynamic_cast<const LineEntity*>(doc.entity(doc.drawOrder()[0]));
    REQUIRE(line);
    REQUIRE(line->layerId() == walls->id);
    REQUIRE(line->p2().x == Approx(100.0));

    const auto* circle = dynamic_cast<const CircleEntity*>(doc.entity(doc.drawOrder()[1]));
    REQUIRE(circle);
    REQUIRE(circle->radius() == Approx(5.0));

    // DXF arc 0..90 degrees CCW.
    const auto* arc = dynamic_cast<const ArcEntity*>(doc.entity(doc.drawOrder()[2]));
    REQUIRE(arc);
    REQUIRE(arc->sweep() == Approx(M_PI_2).margin(1e-6));

    // Closed LWPOLYLINE with a bulge on the second vertex.
    const auto* pl = dynamic_cast<const PolylineEntity*>(doc.entity(doc.drawOrder()[3]));
    REQUIRE(pl);
    REQUIRE(pl->isClosed());
    REQUIRE(pl->vertices().size() == 3);
    REQUIRE(pl->vertices()[1].bulge == Approx(1.0));
}

TEST_CASE("DXF import: missing file fails cleanly", "[dxf]")
{
    const DxfImportResult r = importDxf(QStringLiteral("/nonexistent/nope.dxf"));
    REQUIRE_FALSE(r.ok);
    REQUIRE_FALSE(r.error.isEmpty());
}

TEST_CASE("MTEXT inline formatting decodes to plain text", "[dxf][mtext]")
{
    using namespace viki;
    CHECK(decodeMtextContent(QStringLiteral("ABC\\PDEF")) ==
          QStringLiteral("ABC\nDEF"));
    CHECK(decodeMtextContent(
              QStringLiteral("{\\fArial|b0|i0|c0|p34;Hello} World")) ==
          QStringLiteral("Hello World"));
    CHECK(decodeMtextContent(QStringLiteral("\\H2.5x;Big\\~text")) ==
          QStringLiteral("Big text"));
    CHECK(decodeMtextContent(QStringLiteral("\\S1^2;")) == QStringLiteral("1/2"));
    CHECK(decodeMtextContent(QStringLiteral("\\\\lit \\{b\\}")) ==
          QStringLiteral("\\lit {b}"));
    CHECK(decodeMtextContent(QStringLiteral("\\A1;\\C3;red middle")) ==
          QStringLiteral("red middle"));
    CHECK(decodeMtextContent(QStringLiteral("\\U+00E9t\\U+00E9")) ==
          QStringLiteral("été"));
    CHECK(decodeTextSymbols(QStringLiteral("50%%d %%c10 %%p0.1 100%%%")) ==
          QStringLiteral("50° Ø10 ±0.1 100%"));
}

namespace {

const char* kTextFixture = R"(0
SECTION
2
ENTITIES
0
MTEXT
8
0
10
100.0
20
50.0
40
2.5
71
5
44
1.2
1
{\fArial|b0;Hello \H3.2x;world}\Pline 2
0
TEXT
8
0
10
0.0
20
0.0
40
3.5
1
50%%d and %%c10
50
0.0
72
1
11
30.0
21
40.0
73
3
0
ENDSEC
0

TEST_CASE("MTEXT inline formatting decodes to plain text", "[dxf][mtext]")
{
    using namespace viki;
    CHECK(decodeMtextContent(QStringLiteral("ABC\\PDEF")) ==
          QStringLiteral("ABC\nDEF"));
    CHECK(decodeMtextContent(
              QStringLiteral("{\\fArial|b0|i0|c0|p34;Hello} World")) ==
          QStringLiteral("Hello World"));
    CHECK(decodeMtextContent(QStringLiteral("\\H2.5x;Big\\~text")) ==
          QStringLiteral("Big text"));
    CHECK(decodeMtextContent(QStringLiteral("\\S1^2;")) == QStringLiteral("1/2"));
    CHECK(decodeMtextContent(QStringLiteral("\\\\lit \\{b\\}")) ==
          QStringLiteral("\\lit {b}"));
    CHECK(decodeMtextContent(QStringLiteral("\\A1;\\C3;red middle")) ==
          QStringLiteral("red middle"));
    CHECK(decodeMtextContent(QStringLiteral("\\U+00E9t\\U+00E9")) ==
          QStringLiteral("été"));
    CHECK(decodeTextSymbols(QStringLiteral("50%%d %%c10 %%p0.1 100%%%")) ==
          QStringLiteral("50° Ø10 ±0.1 100%"));
}

namespace {

const char* kTextFixture = R"(0
SECTION
2
ENTITIES
0
MTEXT
8
0
10
100.0
20
50.0
40
2.5
71
5
44
1.2
1
{\fArial|b0;Hello \H3.2x;world}\Pline 2
0
TEXT
8
0
10
0.0
20
0.0
40
3.5
1
50%%d and %%c10
50
0.0
72
1
11
30.0
21
40.0
73
3
0
ENDSEC
0
EOF
)";

} // namespace

TEST_CASE("DXF import: text justification and MTEXT attachment", "[dxf][mtext]")
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("text.dxf"));
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(kTextFixture);
    }

    const DxfImportResult r = importDxf(path);
    REQUIRE(r.ok);
    REQUIRE(r.imported == 2);
    Document& doc = *r.document;

    const auto* mt =
        dynamic_cast<const TextEntity*>(doc.entity(doc.drawOrder()[0]));
    REQUIRE(mt);
    CHECK(mt->text() == QStringLiteral("Hello world\nline 2"));
    CHECK(mt->position().x == Approx(100.0));
    CHECK(mt->height() == Approx(2.5));
    CHECK(mt->hAlign == TextHAlign::Center);   // attachment 5 = MiddleCenter
    CHECK(mt->vAlign == TextVAlign::Middle);
    CHECK(mt->lineSpacing == Approx(2.0));     // 1.2 * 5/3

    const auto* tx =
        dynamic_cast<const TextEntity*>(doc.entity(doc.drawOrder()[1]));
    REQUIRE(tx);
    CHECK(tx->text() == QStringLiteral("50° and Ø10"));
    // Justified TEXT anchors at the alignment point (code 11).
    CHECK(tx->position().x == Approx(30.0));
    CHECK(tx->position().y == Approx(40.0));
    CHECK(tx->hAlign == TextHAlign::Center);
    CHECK(tx->vAlign == TextVAlign::Top);
}

TEST_CASE("DXF reader rejoins dwg2dxf raw-newline value spills", "[dxf][mtext][reader]")
{
    // Reproduce exactly what LibreDWG's dwg2dxf does: it wraps a long MTEXT
    // value at 254 bytes by inserting a raw CR/LF *mid-value*; the spill-over
    // physical line then carries no group code. The reader must rejoin it
    // byte-for-byte (dropping the inserted newline), not drop it.
    QByteArray dxf;
    auto add = [&](const QByteArray& s) { dxf += s; dxf += "\r\n"; };
    add("0"); add("SECTION");
    add("2"); add("ENTITIES");
    add("0"); add("MTEXT");
    add("8"); add("0");
    add("10"); add("0.0");
    add("20"); add("0.0");
    add("40"); add("2.5");
    add("1");
    // 254-byte first segment, then a raw-newline spill (no code) ending in an
    // accented char + more text — the "protégé" truncation shape.
    QByteArray seg(254, 'A');
    dxf += seg;
    dxf += "\r\n";                       // the wrap dwg2dxf injects mid-value
    dxf += QByteArray::fromStdString("prot\xc3\xa9g\xc3\xa9 tail"); // "protégé tail"
    dxf += "\r\n";
    add("7"); add("Standard");           // next real group code resumes parsing
    add("0"); add("ENDSEC");
    add("0"); add("EOF");

    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("spill.dxf"));
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(dxf);
    }

    const DxfImportResult r = importDxf(path);
    REQUIRE(r.ok);
    REQUIRE(r.imported == 1);
    const auto* t =
        dynamic_cast<const TextEntity*>(r.document->entity(r.document->drawOrder()[0]));
    REQUIRE(t);
    // The value is the two physical lines joined with no separator.
    const QString expected =
        QString(254, QLatin1Char('A')) + QStringLiteral("protégé tail");
    CHECK(t->text() == expected);
    CHECK(t->text().endsWith(QStringLiteral("protégé tail")));
}

TEST_CASE("DXF reader leaves normal short values untouched", "[dxf][reader]")
{
    // A value shorter than the wrap width must never absorb the following
    // group-code line — guards against regressions on well-formed DXF.
    QByteArray dxf;
    auto add = [&](const QByteArray& s) { dxf += s; dxf += "\r\n"; };
    add("0"); add("SECTION");
    add("2"); add("ENTITIES");
    add("0"); add("TEXT");
    add("8"); add("0");
    add("10"); add("1.0");
    add("20"); add("2.0");
    add("40"); add("3.5");
    add("1"); add("short");
    add("0"); add("ENDSEC");
    add("0"); add("EOF");

    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("short.dxf"));
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(dxf);
    }
    const DxfImportResult r = importDxf(path);
    REQUIRE(r.ok);
    REQUIRE(r.imported == 1);
    const auto* t =
        dynamic_cast<const TextEntity*>(r.document->entity(r.document->drawOrder()[0]));
    REQUIRE(t);
    CHECK(t->text() == QStringLiteral("short"));
    CHECK(t->position().x == Approx(1.0));
    CHECK(t->position().y == Approx(2.0));
    CHECK(t->height() == Approx(3.5));
}

TEST_CASE("DXF import: ellipse with negative extrusion sweeps the correct half",
          "[dxf][ellipse]")
{
    // Half ellipse, params [pi, 2pi], horizontal major, extrusion (0,0,-1).
    // The OCS minor axis points -Y, so the correct WCS arc is the TOP half
    // (y > center). Without the extrusion fix we'd draw the bottom half.
    QByteArray dxf;
    auto add = [&](const QByteArray& s) { dxf += s; dxf += "\r\n"; };
    add("0"); add("SECTION");
    add("2"); add("ENTITIES");
    add("0"); add("ELLIPSE");
    add("8"); add("0");
    add("10"); add("0.0");
    add("20"); add("0.0");
    add("11"); add("10.0");
    add("21"); add("0.0");
    add("40"); add("0.5");
    add("41"); add("3.141592653589793");
    add("42"); add("6.283185307179586");
    add("210"); add("0.0");
    add("220"); add("0.0");
    add("230"); add("-1.0");
    add("0"); add("ENDSEC");
    add("0"); add("EOF");

    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("ell.dxf"));
    { QFile f(path); REQUIRE(f.open(QIODevice::WriteOnly)); f.write(dxf); }

    const DxfImportResult r = importDxf(path);
    REQUIRE(r.ok);
    REQUIRE(r.imported == 1);
    const auto* e =
        dynamic_cast<const EllipseEntity*>(r.document->entity(r.document->drawOrder()[0]));
    REQUIRE(e);
    const double mid = 0.5 * (e->startParam() + e->endParam());
    CHECK(e->pointAt(mid).y > 0.0); // top half
}
