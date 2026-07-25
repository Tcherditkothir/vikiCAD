#include "StlIo.h"

#include <algorithm>

#include <QFileInfo>

#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <Bnd_Box.hxx>
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
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include "io/OcctMessages.h"
#include "solid/SolidEntity.h"

namespace viki {

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

    for (TopExp_Explorer exp(sewn, TopAbs_FACE); exp.More(); exp.Next())
        ++result.faces;
    if (result.faces == 0) {
        result.error = QStringLiteral("sewing produced no face");
        return result;
    }

    // A closed shell can become a solid; an open one (a mesh with holes, which
    // is common in downloaded parts) stays a shell. Both are real geometry, so
    // report which one came out instead of failing on the open case.
    result.shape = sewn;
    if (sewn.ShapeType() == TopAbs_SHELL) {
        const TopoDS_Shell shell = TopoDS::Shell(sewn);
        if (BRep_Tool::IsClosed(shell)) {
            BRepBuilderAPI_MakeSolid mk(shell);
            if (mk.IsDone() && !mk.Shape().IsNull()) {
                result.shape = mk.Shape();
                result.closed = true;
            }
        }
    }

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
