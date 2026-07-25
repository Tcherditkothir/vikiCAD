#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <QFile>
#include <QTemporaryDir>

#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>

#include "cmd/CommandProcessor.h"
#include "doc/Document.h"
#include "doc/SelectionSet.h"
#include "io/NativeStore.h"
#include "io/StlIo.h"
#include "solid/SolidEntity.h"
#include "solid/SolidMetrics.h"

using namespace viki;
using Catch::Matchers::WithinAbs;

namespace {
struct Rig {
    Document doc;
    SelectionSet selection;
    CommandContext ctx{doc, selection};
    CommandProcessor processor{ctx};
    Rig() { registerBuiltinCommands(processor); }
    bool run(const QString& s) { return processor.submit(s, true).ok; }
};

// A 10x10x10 box: RECT then EXTRUDE 10. Twelve triangles, corner at the origin.
void buildBox(Rig& rig)
{
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 10,10")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 1")));
}

// Export a box to STL and hand back the path, so import tests start from a file
// we know the exact contents of (12 facets, 10mm cube at the origin).
QString writeBoxStl(const QTemporaryDir& dir, const QString& name, bool ascii)
{
    Rig rig;
    buildBox(rig);
    const QString path = dir.filePath(name);
    const StlResult w = exportStl(rig.doc, path, 0.1, ascii);
    REQUIRE(w.ok);
    return path;
}

const SolidEntity* soleSolid(const Document& doc)
{
    const SolidEntity* found = nullptr;
    int count = 0;
    for (const EntityId id : doc.drawOrder()) {
        if (const auto* s = dynamic_cast<const SolidEntity*>(doc.entity(id))) {
            found = s;
            ++count;
        }
    }
    CHECK(count == 1);
    return found;
}
} // namespace

TEST_CASE("importStl reads a binary STL into one mesh-backed solid", "[stl][import]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = writeBoxStl(dir, QStringLiteral("box.stl"), /*ascii=*/false);

    std::unique_ptr<Document> doc;
    const StlResult r = importStl(path, doc);
    REQUIRE(r.ok);
    REQUIRE(doc != nullptr);
    CHECK(r.solids == 1);
    CHECK(r.triangles == 12);

    const SolidEntity* solid = soleSolid(*doc);
    REQUIRE(solid != nullptr);
    CHECK(isMeshShape(solid->shape()));
    CHECK(meshTriangleCount(solid->shape()) == 12);

    // Geometry survives: the 10mm cube keeps its extent and its origin corner.
    CHECK_THAT(solid->bounds().min.x, WithinAbs(0.0, 1e-6));
    CHECK_THAT(solid->bounds().min.y, WithinAbs(0.0, 1e-6));
    CHECK_THAT(solid->bounds().max.x, WithinAbs(10.0, 1e-6));
    CHECK_THAT(solid->bounds().max.y, WithinAbs(10.0, 1e-6));
    CHECK_THAT(solid->zMin(), WithinAbs(0.0, 1e-6));
    CHECK_THAT(solid->zMax(), WithinAbs(10.0, 1e-6));
}

TEST_CASE("importStl detects the ASCII dialect from content, not extension",
          "[stl][import]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    // Deliberately misleading name: the dialect must come from the bytes.
    const QString path = writeBoxStl(dir, QStringLiteral("ascii.bin"), /*ascii=*/true);

    std::unique_ptr<Document> doc;
    const StlResult r = importStl(path, doc);
    REQUIRE(r.ok);
    CHECK(r.triangles == 12);
    CHECK(isMeshShape(soleSolid(*doc)->shape()));
}

TEST_CASE("a mesh-backed face carries no edge, so the slow HLR pass is skipped",
          "[stl][import]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = writeBoxStl(dir, QStringLiteral("box.stl"), false);

    std::unique_ptr<Document> doc;
    REQUIRE(importStl(path, doc).ok);

    // This is what keeps a 30k-triangle import from freezing the 2D canvas:
    // SolidEntity::updateCache() only runs the HLR projection when the shape
    // has between 1 and 4000 edges. A triangulation-only face has none, so the
    // silhouette pass never starts and the canvas falls back to the bbox.
    int edges = 0;
    for (TopExp_Explorer e(soleSolid(*doc)->shape(), TopAbs_EDGE); e.More(); e.Next())
        ++edges;
    CHECK(edges == 0);
}

TEST_CASE("an imported mesh survives a .vkd save/load round-trip", "[stl][import]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString stl = writeBoxStl(dir, QStringLiteral("box.stl"), false);

    std::unique_ptr<Document> doc;
    REQUIRE(importStl(stl, doc).ok);

    // BinTools::Write(shape, stream) passes withTriangles=true, so the mesh is
    // meant to ride the brep blob. Assert it, because a mesh-only shape whose
    // triangulation is dropped deserializes as an EMPTY face -- the model would
    // vanish on reopen with no error anywhere.
    const QString vkd = dir.filePath(QStringLiteral("mesh.vkd"));
    QString error;
    REQUIRE(NativeStore::save(*doc, vkd, error));
    const auto loaded = NativeStore::load(vkd, error);
    REQUIRE(loaded != nullptr);

    const SolidEntity* solid = soleSolid(*loaded);
    REQUIRE(solid != nullptr);
    CHECK(isMeshShape(solid->shape()));
    CHECK(meshTriangleCount(solid->shape()) == 12);
    CHECK_THAT(solid->zMax(), WithinAbs(10.0, 1e-6));
}

TEST_CASE("isMeshShape tells a mesh apart from real BREP geometry", "[stl][import]")
{
    Rig rig;
    buildBox(rig);
    const SolidEntity* extruded = soleSolid(rig.doc);
    REQUIRE(extruded != nullptr);
    // An extruded box has planar surfaces: not a mesh, so booleans and fillets
    // stay available on it.
    CHECK_FALSE(isMeshShape(extruded->shape()));
    CHECK(meshTriangleCount(extruded->shape()) == 0);

    CHECK_FALSE(isMeshShape(TopoDS_Shape()));
}

TEST_CASE("meshToSolid sews a meshed box back into a watertight solid",
          "[stl][import][mesh2solid]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = writeBoxStl(dir, QStringLiteral("box.stl"), false);

    std::unique_ptr<Document> doc;
    REQUIRE(importStl(path, doc).ok);
    const TopoDS_Shape mesh = soleSolid(*doc)->shape();

    const MeshToSolidResult r = meshToSolid(mesh);
    REQUIRE(r.ok);
    CHECK(r.error.isEmpty());
    CHECK(r.triangles == 12);
    CHECK(r.closed);
    // Twelve triangles stay twelve planar faces: sewing joins them, it does not
    // merge coplanar pairs back into the 6 original box faces.
    CHECK(r.faces == 12);

    // The real proof that the geometry survived: a 10mm cube is 1000 mm3. A
    // mis-sewn or inside-out shell would give 0 or a negative volume.
    REQUIRE_FALSE(r.shape.IsNull());
    const auto m = solidops::solidMetrics(r.shape);
    REQUIRE(m.valid);
    CHECK_THAT(m.volume, WithinAbs(1000.0, 1e-6));
    CHECK_THAT(m.area, WithinAbs(600.0, 1e-6));

    // And it is no longer a mesh, so booleans and sections now apply to it.
    CHECK_FALSE(isMeshShape(r.shape));
}

TEST_CASE("meshToSolid refuses what it cannot convert", "[stl][import][mesh2solid]")
{
    SECTION("real BREP geometry is not a mesh")
    {
        Rig rig;
        buildBox(rig);
        const MeshToSolidResult r = meshToSolid(soleSolid(rig.doc)->shape());
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("not a mesh")));
        CHECK(r.shape.IsNull());
    }

    SECTION("a null shape")
    {
        const MeshToSolidResult r = meshToSolid(TopoDS_Shape());
        CHECK_FALSE(r.ok);
        CHECK(r.shape.IsNull());
    }

    SECTION("over the triangle cap, it refuses instead of hanging")
    {
        QTemporaryDir dir;
        REQUIRE(dir.isValid());
        const QString path = writeBoxStl(dir, QStringLiteral("box.stl"), false);
        std::unique_ptr<Document> doc;
        REQUIRE(importStl(path, doc).ok);

        // Cap below the box's 12 triangles: the refusal must name both numbers
        // so the message tells you how far over you are.
        const MeshToSolidResult r = meshToSolid(soleSolid(*doc)->shape(), 5);
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("12")));
        CHECK(r.error.contains(QStringLiteral("5")));
        CHECK(r.shape.IsNull());
    }
}

TEST_CASE("the MESH2SOLID command replaces the mesh in one undoable step",
          "[stl][import][mesh2solid]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = writeBoxStl(dir, QStringLiteral("box.stl"), false);
    std::unique_ptr<Document> imported;
    REQUIRE(importStl(path, imported).ok);
    const TopoDS_Shape mesh = soleSolid(*imported)->shape();

    Rig rig;
    const EntityId id = rig.doc.nextId();
    rig.doc.restoreEntity(std::make_unique<SolidEntity>(mesh), id);
    rig.doc.setNextId(id + 1);
    REQUIRE(isMeshShape(soleSolid(rig.doc)->shape()));

    REQUIRE(rig.run(QStringLiteral("MESH2SOLID %1").arg(id)));

    const SolidEntity* after = soleSolid(rig.doc);
    REQUIRE(after != nullptr);
    CHECK_FALSE(isMeshShape(after->shape()));
    CHECK_THAT(solidops::solidMetrics(after->shape()).volume, WithinAbs(1000.0, 1e-6));

    // One transaction, so one UNDO puts the mesh back -- a batch that left the
    // document half-converted would need several.
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    CHECK(isMeshShape(soleSolid(rig.doc)->shape()));
}

TEST_CASE("MESH2SOLID refuses geometry that is not a mesh", "[stl][import][mesh2solid]")
{
    Rig rig;
    buildBox(rig);
    EntityId id = kInvalidEntityId;
    for (const EntityId e : rig.doc.drawOrder())
        if (dynamic_cast<const SolidEntity*>(rig.doc.entity(e)))
            id = e;
    REQUIRE(id != kInvalidEntityId);
    const int facesBefore = [&] {
        int n = 0;
        for (TopExp_Explorer e(soleSolid(rig.doc)->shape(), TopAbs_FACE); e.More();
             e.Next())
            ++n;
        return n;
    }();
    REQUIRE(facesBefore == 6);

    rig.run(QStringLiteral("MESH2SOLID %1").arg(id));

    // The refusal says why, in words that name the actual situation.
    bool explained = false;
    for (const QString& m : rig.ctx.messages())
        explained = explained || m.contains(QStringLiteral("not a mesh"));
    CHECK(explained);

    // And the box is untouched: still 6 real faces, not 12 sewn facets.
    int facesAfter = 0;
    for (TopExp_Explorer e(soleSolid(rig.doc)->shape(), TopAbs_FACE); e.More(); e.Next())
        ++facesAfter;
    CHECK(facesAfter == 6);
    CHECK_THAT(solidops::solidMetrics(soleSolid(rig.doc)->shape()).volume,
               WithinAbs(1000.0, 1e-6));
}

TEST_CASE("importStl fails cleanly on a missing or bogus file", "[stl][import]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());

    SECTION("missing file")
    {
        std::unique_ptr<Document> doc;
        const StlResult r = importStl(dir.filePath(QStringLiteral("nope.stl")), doc);
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("no such file")));
        CHECK(doc == nullptr);
    }

    SECTION("not an STL at all")
    {
        const QString path = dir.filePath(QStringLiteral("garbage.stl"));
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("this is not a mesh, it is a sentence\n");
        f.close();

        std::unique_ptr<Document> doc;
        const StlResult r = importStl(path, doc);
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.error.isEmpty());
        CHECK(doc == nullptr);
    }
}
