#include "StlIo.h"

#include <algorithm>

#include <QFileInfo>
#include <QStringList>

#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepLib.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Poly_Triangulation.hxx>
#include <Precision.hxx>
#include <RWStl.hxx>
#include <StlAPI_Writer.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include "io/OcctMessages.h"
#include "solid/SolidEntity.h"

namespace viki {

namespace {

// Signed volume of a shape (GProp volume properties, mm³). The SIGN carries
// meaning here: a closed shell whose faces wind inward measures negative —
// see meshToSolid, which normalises that (issue #8).
double signedVolume(const TopoDS_Shape& shape)
{
    if (shape.IsNull())
        return 0.0;
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props);
    return props.Mass();
}

} // namespace

StlResult exportStl(const Document& doc, const QString& path, double deflection,
                    bool ascii)
{
    StlResult result;

    if (deflection <= 0.0)
        deflection = 0.1;

    // Gather every solid into a single compound so the whole model lands in
    // one STL file (STL has no concept of separate objects anyway).
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);

    int solids = 0;
    for (const EntityId id : doc.drawOrder()) {
        const auto* solid = dynamic_cast<const SolidEntity*>(doc.entity(id));
        if (!solid || solid->shape().IsNull())
            continue;
        builder.Add(compound, solid->shape());
        ++solids;
    }

    if (solids == 0) {
        result.error =
            QStringLiteral("no solids to export (EXTRUDE/REVOLVE first)");
        return result;
    }

    // Triangulate. IncrementalMesh mutates the shape's triangulation in place;
    // Perform() must run before the writer can emit facets.
    BRepMesh_IncrementalMesh mesher(compound, deflection, Standard_False, 0.5,
                                    Standard_True);
    mesher.Perform();

    StlAPI_Writer writer;
    writer.ASCIIMode() = ascii ? Standard_True : Standard_False;
    if (!writer.Write(compound, path.toUtf8().constData())) {
        result.error = QStringLiteral("cannot write STL to %1").arg(path);
        return result;
    }

    result.ok = true;
    result.solids = solids;
    return result;
}

bool isMeshShape(const TopoDS_Shape& shape)
{
    if (shape.IsNull())
        return false;
    // A mesh-backed face has a triangulation but no surface. Any face with a
    // real surface (extrusion, revolution, imported STEP) fails the second
    // test, so a shape only counts as a mesh when EVERY face is surface-less.
    int faces = 0;
    for (TopExp_Explorer exp(shape, TopAbs_FACE); exp.More(); exp.Next()) {
        const TopoDS_Face face = TopoDS::Face(exp.Current());
        TopLoc_Location loc;
        if (BRep_Tool::Surface(face, loc).IsNull()) {
            if (BRep_Tool::Triangulation(face, loc).IsNull())
                return false; // neither surface nor mesh: not ours
            ++faces;
        } else {
            return false;
        }
    }
    return faces > 0;
}

int meshTriangleCount(const TopoDS_Shape& shape)
{
    if (shape.IsNull())
        return 0;
    int total = 0;
    for (TopExp_Explorer exp(shape, TopAbs_FACE); exp.More(); exp.Next()) {
        TopLoc_Location loc;
        const Handle(Poly_Triangulation) tri =
            BRep_Tool::Triangulation(TopoDS::Face(exp.Current()), loc);
        if (!tri.IsNull())
            total += tri->NbTriangles();
    }
    return total;
}

StlResult importStl(const QString& path, std::unique_ptr<Document>& outDoc)
{
    StlResult result;
    // A truncated STL makes OCCT print "premature end of file" on stdout, which
    // would corrupt the CLI's JSON reply.
    silenceOcctMessages();

    // RWStl reports a missing file the same way as a malformed one (null
    // handle), so check existence first to keep the two errors apart.
    if (!QFileInfo::exists(path)) {
        result.error = QStringLiteral("no such file: %1").arg(path);
        return result;
    }

    // M_PI/2 = "ignore the angle", i.e. weld every coincident vertex. STL
    // repeats each vertex once per touching triangle; welding turns that soup
    // into a connected mesh and cuts the node count roughly by six.
    const Handle(Poly_Triangulation) tri =
        RWStl::ReadFile(path.toUtf8().constData(), M_PI / 2.0);
    if (tri.IsNull()) {
        result.error =
            QStringLiteral("cannot read STL %1 (not a valid ASCII or binary STL)")
                .arg(path);
        return result;
    }
    if (tri->NbTriangles() == 0) {
        result.error = QStringLiteral("STL %1 holds no triangles").arg(path);
        return result;
    }

    // One face, one triangulation, no surface. See the header for why this is
    // the right shape for a mesh.
    TopoDS_Face face;
    BRep_Builder builder;
    builder.MakeFace(face, tri);

    outDoc = std::make_unique<Document>();
    outDoc->restoreEntity(std::make_unique<SolidEntity>(face), outDoc->nextId());
    outDoc->setNextId(outDoc->nextId() + 1);

    result.ok = true;
    result.solids = 1;
    result.triangles = tri->NbTriangles();
    // Issue #4: mesh-backed solids cannot cross STEP (the XCAF writer emits
    // an empty MANIFOLD_SURFACE_SHAPE_REPRESENTATION for a surface-less face
    // and the file loses the geometry). Warn here, at import, where the cure
    // is one command away — the implicit alternative changes the part and is
    // capped at 20 000 triangles, so the warning names MESH2SOLID instead.
    result.warnings << QStringLiteral(
        "this STL imported as a MESH (triangulation only): viewing, "
        "measuring and moving are fine, but STEP export of a mesh writes no "
        "geometry — run MESH2SOLID <id> first if you need a STEP (booleans, "
        "fillets also need real geometry)");
    return result;
}

MeshToSolidResult meshToSolid(const TopoDS_Shape& mesh, int maxTriangles)
{
    MeshToSolidResult result;
    silenceOcctMessages();

    if (!isMeshShape(mesh)) {
        result.error = QStringLiteral("not a mesh (nothing to convert)");
        return result;
    }
    result.triangles = meshTriangleCount(mesh);
    if (result.triangles > maxTriangles) {
        result.error =
            QStringLiteral("mesh has %1 triangles, over the %2 limit — sewing "
                           "50k took 39 s and 800 MB here, and a solid with "
                           "that many faces stays slow forever. Keep it a mesh, "
                           "or raise VIKICAD_MESH2SOLID_MAX if you accept the "
                           "cost.")
                .arg(result.triangles)
                .arg(maxTriangles);
        return result;
    }

    // Sewing tolerance: RWStl already welded coincident vertices, so triangles
    // that touch share bit-identical nodes and a tight tolerance suffices. Scale
    // it to the model anyway, so a part modelled in metres sews as readily as
    // one in millimetres.
    Bnd_Box box;
    BRepBndLib::Add(mesh, box);
    double tol = Precision::Confusion();
    if (!box.IsVoid()) {
        double xmin, ymin, zmin, xmax, ymax, zmax;
        box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
        const gp_XYZ span(xmax - xmin, ymax - ymin, zmax - zmin);
        tol = std::max(tol, 1e-6 * span.Modulus());
    }

    BRepBuilderAPI_Sewing sewer(tol);
    int fed = 0, degenerate = 0;
    for (TopExp_Explorer exp(mesh, TopAbs_FACE); exp.More(); exp.Next()) {
        TopLoc_Location loc;
        const Handle(Poly_Triangulation) tri =
            BRep_Tool::Triangulation(TopoDS::Face(exp.Current()), loc);
        if (tri.IsNull())
            continue;
        const gp_Trsf trsf = loc.Transformation();
        for (int t = 1; t <= tri->NbTriangles(); ++t) {
            int a = 0, b = 0, c = 0;
            tri->Triangle(t).Get(a, b, c);
            gp_Pnt pa = tri->Node(a).Transformed(trsf);
            gp_Pnt pb = tri->Node(b).Transformed(trsf);
            gp_Pnt pc = tri->Node(c).Transformed(trsf);

            // A zero-area triangle has no plane, so MakeFace would fail and
            // abort the whole conversion. Slicer exports carry a few; drop them
            // and report the count rather than dying on them.
            const gp_Vec normal = gp_Vec(pa, pb).Crossed(gp_Vec(pa, pc));
            if (normal.Magnitude() <= tol * tol) {
                ++degenerate;
                continue;
            }

            BRepBuilderAPI_MakePolygon poly(pa, pb, pc, /*Close=*/Standard_True);
            if (!poly.IsDone())
                continue;
            BRepBuilderAPI_MakeFace face(poly.Wire(), /*OnlyPlane=*/Standard_True);
            if (!face.IsDone())
                continue;
            sewer.Add(face.Face());
            ++fed;
        }
    }

    if (fed == 0) {
        result.error = QStringLiteral("no usable triangle in the mesh");
        return result;
    }

    sewer.Perform();
    const TopoDS_Shape sewn = sewer.SewedShape();
    if (sewn.IsNull()) {
        result.error = QStringLiteral("sewing produced nothing");
        return result;
    }

    // --- Issue #8: walk the sewn shape SHELL BY SHELL ------------------------
    //
    // The old code only looked at a top-level TopAbs_SHELL, so anything the
    // sewer handed back as a COMPOUND (which is what a mesh of two overlapping
    // bodies sews into: two disjoint closed shells, no shared vertex) fell
    // through to `result.shape = sewn` with closed=false and was reported as an
    // "open shell" even though both halves were watertight. And a top-level
    // shell whose triangles wound INWARD produced a solid with a NEGATIVE
    // volume, which the caller then read as anti-material (magnitudes looked
    // plausible, so it survived review).
    //
    // Both are fixed here, per shell:
    //   1. a closed shell becomes a solid;
    //   2. if that solid's signed volume is negative, its faces point at the
    //      inside, so it is flipped;
    //   3. everything (solids and any genuinely open shell) is collected into
    //      one COMPOUND — nothing is ever dropped.
    std::vector<TopoDS_Shape> parts;
    for (TopExp_Explorer exp(sewn, TopAbs_SHELL); exp.More(); exp.Next()) {
        const TopoDS_Shell shell = TopoDS::Shell(exp.Current());
        if (!BRep_Tool::IsClosed(shell)) {
            // A mesh with holes is common in downloaded parts and is still real
            // geometry — keep the shell as-is rather than failing on it.
            ++result.openShells;
            parts.push_back(shell);
            continue;
        }

        BRepBuilderAPI_MakeSolid mk(shell);
        if (!mk.IsDone() || mk.Shape().IsNull()) {
            // IsClosed said yes but MakeSolid would not commit: keep the shell
            // instead of losing the geometry, and count it honestly.
            ++result.openShells;
            parts.push_back(shell);
            continue;
        }
        TopoDS_Solid solid = mk.Solid();

        // The signed volume is the whole point of the issue: sewing carries the
        // input winding through untouched, so an inward-wound mesh yields a
        // solid enclosing "negative" material. BRepLib::OrientClosedSolid
        // restores outward orientation when it finds the faces pointing in.
        if (signedVolume(solid) < 0.0) {
            BRepLib::OrientClosedSolid(solid);
            if (signedVolume(solid) < 0.0) {
                // Belt and braces: an explicit shell-level reversal, for the
                // rare shape OrientClosedSolid declines to touch.
                std::vector<TopoDS_Shape> flipped;
                for (TopExp_Explorer f(solid, TopAbs_FACE); f.More(); f.Next())
                    flipped.push_back(f.Current().Reversed());
                TopoDS_Shell rebuilt;
                BRep_Builder shellBuilder;
                shellBuilder.MakeShell(rebuilt);
                for (const TopoDS_Shape& f : flipped)
                    shellBuilder.Add(rebuilt, f);
                BRepBuilderAPI_MakeSolid remade(rebuilt);
                if (remade.IsDone() && !remade.Shape().IsNull())
                    solid = remade.Solid();
            }
            ++result.reoriented;
        }
        ++result.solids;
        parts.push_back(solid);
    }

    if (parts.empty()) {
        result.error = QStringLiteral("sewing produced no shell");
        return result;
    }

    // One shell comes back as itself (a bare solid, not wrapped in a compound:
    // that is the shape every earlier consumer and test saw). Several come back
    // as a compound.
    result.shape = parts.size() == 1 ? parts.front() : [&parts] {
        TopoDS_Compound comp;
        BRep_Builder builder;
        builder.MakeCompound(comp);
        for (const TopoDS_Shape& p : parts)
            builder.Add(comp, p);
        return TopoDS_Shape(comp);
    }();

    for (TopExp_Explorer exp(result.shape, TopAbs_FACE); exp.More(); exp.Next())
        ++result.faces;
    if (result.faces == 0) {
        result.error = QStringLiteral("sewing produced no face");
        return result;
    }
    // "closed" keeps its old meaning — a watertight solid came out — but now it
    // is only true when EVERY shell made it.
    result.closed = result.openShells == 0 && result.solids > 0;

    if (result.shape.IsNull()) {
        result.error = QStringLiteral("conversion produced a null shape");
        return result;
    }
    if (degenerate > 0)
        result.warning = QStringLiteral("%1 zero-area triangle(s) skipped")
                             .arg(degenerate);
    result.ok = true;
    return result;
}

} // namespace viki
