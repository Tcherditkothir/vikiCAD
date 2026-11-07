#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstring>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
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

// Binary STL surgery, for the issue #8 fixtures. A binary STL is a 80-byte
// header, a uint32 facet count, then 50 bytes per facet (normal, 3 vertices,
// a uint16 attribute). Both helpers below rewrite that layout directly, so the
// tests drive EXACTLY the geometry they mean to — no exporter opinion in the
// way.
struct StlFacet {
    float n[3];
    float v[3][3];
    quint16 attr = 0;
};

std::vector<StlFacet> readBinaryStl(const QString& path)
{
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QByteArray blob = f.readAll();
    REQUIRE(blob.size() >= 84);
    const char* p = blob.constData();
    quint32 count = 0;
    std::memcpy(&count, p + 80, 4);
    REQUIRE(blob.size() >= 84 + int(count) * 50);
    std::vector<StlFacet> facets;
    for (quint32 i = 0; i < count; ++i) {
        StlFacet s;
        std::memcpy(&s, p + 84 + i * 50, 50);
        facets.push_back(s);
    }
    return facets;
}

void writeBinaryStl(const QString& path, const std::vector<StlFacet>& facets)
{
    QByteArray blob(80, '\0');
    const char* tag = "vikiCAD test fixture";
    std::memcpy(blob.data(), tag, std::strlen(tag));
    const quint32 count = quint32(facets.size());
    blob.append(reinterpret_cast<const char*>(&count), 4);
    for (const StlFacet& s : facets)
        blob.append(reinterpret_cast<const char*>(&s), 50);
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    REQUIRE(f.write(blob) == blob.size());
}

// Reverse every facet's winding: the surface is bit-for-bit the same point set,
// only its orientation flips. This is what an inward-wound export looks like,
// and sewing cannot tell the difference on its own.
void flipWinding(std::vector<StlFacet>& facets)
{
    for (StlFacet& s : facets) {
        std::swap(s.v[1], s.v[2]);
        for (float& c : s.n)
            c = -c;
    }
}

// Two boxes side by side sharing NO vertex: a concatenated union, i.e. two
// independent closed shells in one file (issue #8's "deux veines").
std::vector<StlFacet> twoBoxes(const QString& dir)
{
    Rig a;
    REQUIRE(a.run(QStringLiteral("RECT 0,0 10,10")));
    REQUIRE(a.run(QStringLiteral("EXTRUDE 10 1")));
    const QString one = QDir(dir).filePath(QStringLiteral("box-a.stl"));
    REQUIRE(exportStl(a.doc, one, 0.1, false).ok);

    Rig b;
    REQUIRE(b.run(QStringLiteral("RECT 20,0 30,10")));
    REQUIRE(b.run(QStringLiteral("EXTRUDE 10 1")));
    const QString two = QDir(dir).filePath(QStringLiteral("box-b.stl"));
    REQUIRE(exportStl(b.doc, two, 0.1, false).ok);

    std::vector<StlFacet> merged = readBinaryStl(one);
    const std::vector<StlFacet> other = readBinaryStl(two);
    merged.insert(merged.end(), other.begin(), other.end());
    return merged;
}

int shellCount(const TopoDS_Shape& s)
{
    int n = 0;
    for (TopExp_Explorer e(s, TopAbs_SHELL); e.More(); e.Next())
        ++n;
    return n;
}

int solidCount(const TopoDS_Shape& s)
{
    int n = 0;
    for (TopExp_Explorer e(s, TopAbs_SOLID); e.More(); e.Next())
        ++n;
    return n;
}

// The smallest volume found anywhere in the shape, shell by shell. Used to
// assert "no anti-material survived anywhere" rather than just at the top.
double minShellVolume(const TopoDS_Shape& s)
{
    double lo = 0.0;
    bool first = true;
    for (TopExp_Explorer e(s, TopAbs_SHELL); e.More(); e.Next()) {
        GProp_GProps props;
        BRepGProp::VolumeProperties(e.Current(), props);
        if (first || props.Mass() < lo) {
            lo = props.Mass();
            first = false;
        }
    }
    return first ? 0.0 : lo;
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

TEST_CASE("importStl warns that STEP export of a mesh needs MESH2SOLID first",
          "[stl][import][issue4]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = writeBoxStl(dir, QStringLiteral("box.stl"), false);

    std::unique_ptr<Document> doc;
    const StlResult r = importStl(path, doc);
    REQUIRE(r.ok);

    // Issue #4: a meshed solid crossing STEP loses its geometry (the XCAF
    // writer emits an EMPTY MANIFOLD_SURFACE_SHAPE_REPRESENTATION). The
    // import reply must say so BEFORE the export, and name the cure.
    REQUIRE_FALSE(r.warnings.isEmpty());
    bool warned = false;
    for (const QString& w : r.warnings)
        warned = warned || (w.contains(QStringLiteral("MESH2SOLID")) &&
                            w.contains(QStringLiteral("STEP")));
    CHECK(warned);
}

TEST_CASE("meshToSolid flips a shell whose faces wind inward (issue #8)",
          "[stl][import][mesh2solid][issue8]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString good = writeBoxStl(dir, QStringLiteral("box.stl"), false);

    // The SAME 10mm box, every facet reversed: identical point set, identical
    // bounds, opposite orientation. This is the mesh vivi-dessinateur's trimesh
    // export produced, and it used to come out of MESH2SOLID as a solid whose
    // volume was -1000.
    std::vector<StlFacet> facets = readBinaryStl(good);
    REQUIRE(facets.size() == 12);
    flipWinding(facets);
    const QString insideOut = dir.filePath(QStringLiteral("box-inside-out.stl"));
    writeBinaryStl(insideOut, facets);

    std::unique_ptr<Document> doc;
    REQUIRE(importStl(insideOut, doc).ok);
    const TopoDS_Shape mesh = soleSolid(*doc)->shape();

    // The mesh itself is unchanged geometry — same extents as the good box.
    CHECK_THAT(soleSolid(*doc)->bounds().max.x, WithinAbs(10.0, 1e-6));

    const MeshToSolidResult r = meshToSolid(mesh);
    REQUIRE(r.ok);
    CHECK(r.closed);
    CHECK(r.solids == 1);
    CHECK(r.openShells == 0);
    CHECK(r.reoriented == 1); // the report says the winding had to be flipped
    CHECK(r.faces == 12);

    // THE regression: a positive volume, exactly that of the box.
    REQUIRE_FALSE(r.shape.IsNull());
    CHECK_THAT(solidops::solidMetrics(r.shape).volume, WithinAbs(1000.0, 1e-6));
    // …and no anti-material hiding in any shell of the result.
    CHECK(minShellVolume(r.shape) > 0.0);
}

TEST_CASE("meshToSolid keeps every shell of a concatenated mesh (issue #8)",
          "[stl][import][mesh2solid][issue8]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const std::vector<StlFacet> merged = twoBoxes(dir.path());
    REQUIRE(merged.size() == 24);
    const QString path = dir.filePath(QStringLiteral("two-veines.stl"));
    writeBinaryStl(path, merged);

    std::unique_ptr<Document> doc;
    REQUIRE(importStl(path, doc).ok);
    const MeshToSolidResult r = meshToSolid(soleSolid(*doc)->shape());
    REQUIRE(r.ok);

    // Two disjoint closed shells that share no vertex — the sewer hands back a
    // COMPOUND, and the old code called that an "open shell" and never made a
    // solid of either half. Both are bodies now, and nothing is dropped.
    CHECK(r.faces == 24);
    CHECK(r.solids == 2);
    CHECK(r.openShells == 0);
    CHECK(r.closed);
    CHECK(shellCount(r.shape) == 2);
    CHECK(solidCount(r.shape) == 2);
    CHECK_THAT(solidops::solidMetrics(r.shape).volume, WithinAbs(2000.0, 1e-6));
    CHECK(minShellVolume(r.shape) > 0.0);
}

TEST_CASE("meshToSolid normalises a concatenated mesh of MIXED winding (issue #8)",
          "[stl][import][mesh2solid][issue8]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    std::vector<StlFacet> merged = twoBoxes(dir.path());
    REQUIRE(merged.size() == 24);
    // Second half wound inward: without normalisation the two halves' volumes
    // cancel to ~0 and the part measures as empty space.
    std::vector<StlFacet> second(merged.begin() + 12, merged.end());
    flipWinding(second);
    std::copy(second.begin(), second.end(), merged.begin() + 12);
    const QString path = dir.filePath(QStringLiteral("veines-mixtes.stl"));
    writeBinaryStl(path, merged);

    std::unique_ptr<Document> doc;
    REQUIRE(importStl(path, doc).ok);
    const MeshToSolidResult r = meshToSolid(soleSolid(*doc)->shape());
    REQUIRE(r.ok);

    CHECK(r.solids == 2);
    CHECK(r.reoriented == 1);
    // Both halves now contribute their real 1000 mm3 instead of cancelling.
    CHECK_THAT(solidops::solidMetrics(r.shape).volume, WithinAbs(2000.0, 1e-6));
    CHECK(minShellVolume(r.shape) > 0.0);
}

TEST_CASE("the MESH2SOLID report names the body count and the flip (issue #8)",
          "[stl][import][mesh2solid][issue8]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    std::vector<StlFacet> facets = readBinaryStl(
        writeBoxStl(dir, QStringLiteral("box.stl"), false));
    flipWinding(facets);
    const QString path = dir.filePath(QStringLiteral("box-inside-out.stl"));
    writeBinaryStl(path, facets);

    std::unique_ptr<Document> imported;
    REQUIRE(importStl(path, imported).ok);
    const TopoDS_Shape mesh = soleSolid(*imported)->shape();

    Rig rig;
    const EntityId id = rig.doc.nextId();
    rig.doc.restoreEntity(std::make_unique<SolidEntity>(mesh), id);
    rig.doc.setNextId(id + 1);
    REQUIRE(rig.run(QStringLiteral("MESH2SOLID %1").arg(id)));

    // The old wording said "open shell" for a concatenated mesh that was in
    // fact fully watertight, which read as a failure. Count the bodies, and
    // state plainly when the winding had to be turned around.
    bool bodies = false, flipped = false;
    for (const QString& m : rig.ctx.messages()) {
        bodies = bodies || m.contains(QStringLiteral("1 closed solid"));
        flipped = flipped || m.contains(QStringLiteral("inward-facing"));
    }
    CHECK(bodies);
    CHECK(flipped);
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
