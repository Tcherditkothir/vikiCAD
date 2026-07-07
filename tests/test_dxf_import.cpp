#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <QFile>
#include <QTemporaryDir>

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
