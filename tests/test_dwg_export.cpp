#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <QFile>
#include <QTemporaryDir>

#include "cmd/CommandProcessor.h"
#include "doc/Block.h"
#include "doc/Entities.h"
#include "doc/SelectionSet.h"
#include "io/DwgExporter.h"
#include "io/DxfImporter.h"

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
};

const Entity* findFirst(const Document& doc, const char* type)
{
    for (const EntityId id : doc.drawOrder())
        if (QLatin1String(doc.entity(id)->typeName()) == QLatin1String(type))
            return doc.entity(id);
    return nullptr;
}
} // namespace

TEST_CASE("DWG export -> reimport preserves geometry", "[dwg]")
{
    if (dwgExportTool().isEmpty())
        SKIP("dxf2dwg (GNU LibreDWG) not installed on this machine");

    Rig rig;
    const LayerId walls =
        rig.doc.ensureLayer(QStringLiteral("walls"), 0xFF0000);
    rig.doc.setCurrentLayer(walls);
    REQUIRE(rig.run(QStringLiteral("LINE 0,0 100,50")));
    rig.doc.setCurrentLayer(0);
    REQUIRE(rig.run(QStringLiteral("CIRCLE 10,20 5")));
    REQUIRE(rig.run(QStringLiteral("ARC 30,0 35,5 40,0")));
    REQUIRE(rig.run(QStringLiteral("RECT 50,50 80,90")));
    EntityId rectId = kInvalidEntityId;
    for (const EntityId id : rig.doc.drawOrder())
        rectId = std::max(rectId, id);
    REQUIRE(rig.run(QStringLiteral("HATCH ANSI31 1 %1").arg(rectId)));

    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("rt.dwg"));
    const DwgExportResult ex = exportDwg(rig.doc, path);
    REQUIRE(ex.ok);
    CHECK(ex.exported == 5);
    REQUIRE(QFile::exists(path));

    // Read the DWG back through the OTHER implementation (libdwgr): entities
    // unreachable from *Model_Space would vanish right here — that was the
    // original bug (orphans in the object map, "empty" DWG everywhere).
    const DxfImportResult im = importDwg(path);
    REQUIRE(im.ok);
    REQUIRE(im.imported == 5);
    REQUIRE(findFirst(*im.document, "hatch"));

    Document& d2 = *im.document;
    REQUIRE(d2.layerByName(QStringLiteral("walls")));

    const auto* line = dynamic_cast<const LineEntity*>(findFirst(d2, "line"));
    REQUIRE(line);
    CHECK(line->p2().x == Approx(100.0));
    CHECK(line->p2().y == Approx(50.0));

    const auto* circle =
        dynamic_cast<const CircleEntity*>(findFirst(d2, "circle"));
    REQUIRE(circle);
    CHECK(circle->radius() == Approx(5.0));

    const auto* arc = dynamic_cast<const ArcEntity*>(findFirst(d2, "arc"));
    REQUIRE(arc);
    CHECK(arc->radius() == Approx(5.0).margin(1e-6));
}

TEST_CASE("DWG export keeps block contents reachable", "[dwg]")
{
    if (dwgExportTool().isEmpty())
        SKIP("dxf2dwg (GNU LibreDWG) not installed on this machine");

    Rig rig;
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 10,10")));
    EntityId rectId = kInvalidEntityId;
    for (const EntityId id : rig.doc.drawOrder())
        rectId = std::max(rectId, id);
    // Keyword name, base point, entity set (implicit Finish in strict mode).
    REQUIRE(rig.run(QStringLiteral("BLOCK B1 0,0 %1").arg(rectId)));
    REQUIRE(rig.run(QStringLiteral("INSERT B1 200,0 1 0")));

    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("blocks.dwg"));
    const DwgExportResult ex = exportDwg(rig.doc, path);
    REQUIRE(ex.ok);

    const DxfImportResult im = importDwg(path);
    REQUIRE(im.ok);
    Document& d2 = *im.document;

    // The definition came back with its content (the rectangle polyline is
    // owned by B1's block record — patch 0005 covers BLOCKS too)...
    const BlockDef* def = d2.blockByName(QStringLiteral("B1"));
    REQUIRE(def);
    REQUIRE_FALSE(def->entities.empty());
    // ...and the insert still references it.
    REQUIRE(findFirst(d2, "insert"));
}
