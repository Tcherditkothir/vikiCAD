#include "ObjIo.h"

#include <QFile>
#include <QTextStream>

#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include "solid/SolidEntity.h"

namespace viki {

ObjResult exportObj(const Document& doc, const QString& path, double deflection)
{
    ObjResult result;

    if (deflection <= 0.0)
        deflection = 0.1;

    // Collect the solids up front so we can fail cleanly on an empty model
    // before touching the output file.
    std::vector<TopoDS_Shape> shapes;
    for (const EntityId id : doc.drawOrder()) {
        const auto* solid = dynamic_cast<const SolidEntity*>(doc.entity(id));
        if (!solid || solid->shape().IsNull())
            continue;
        shapes.push_back(solid->shape());
    }

    if (shapes.empty()) {
        result.error =
            QStringLiteral("no solids to export (EXTRUDE/REVOLVE first)");
        return result;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        result.error = QStringLiteral("cannot write OBJ to %1").arg(path);
        return result;
    }

    QTextStream out(&file);
    out << "# Wavefront OBJ exported by VikiCAD\n";
    out << "# units: mm\n";

    // OBJ face indices are 1-based and global across the whole file, so we keep
    // a running offset as each face contributes its own block of vertices.
    int vertexOffset = 0;

    for (const TopoDS_Shape& shape : shapes) {
        // Triangulate. IncrementalMesh mutates the shape's triangulation in
        // place; Perform() must run before we can read the facets back.
        BRepMesh_IncrementalMesh mesher(shape, deflection, Standard_False, 0.5,
                                        Standard_True);
        mesher.Perform();

        for (TopExp_Explorer exp(shape, TopAbs_FACE); exp.More(); exp.Next()) {
            const TopoDS_Face face = TopoDS::Face(exp.Current());
            TopLoc_Location loc;
            const Handle(Poly_Triangulation) tri =
                BRep_Tool::Triangulation(face, loc);
            if (tri.IsNull() || tri->NbTriangles() == 0)
                continue;

            const gp_Trsf trsf = loc.Transformation();
            const bool reversed = face.Orientation() == TopAbs_REVERSED;

            // Emit this face's vertices (and per-vertex normals when present),
            // transformed into world coordinates.
            const int nbNodes = tri->NbNodes();
            const bool hasNormals = tri->HasNormals();
            for (int i = 1; i <= nbNodes; ++i) {
                gp_Pnt p = tri->Node(i);
                p.Transform(trsf);
                out << "v " << p.X() << ' ' << p.Y() << ' ' << p.Z() << '\n';
                if (hasNormals) {
                    gp_Dir n = tri->Normal(i);
                    gp_Vec nv(n.X(), n.Y(), n.Z());
                    nv.Transform(trsf);
                    if (reversed)
                        nv.Reverse();
                    out << "vn " << nv.X() << ' ' << nv.Y() << ' ' << nv.Z()
                        << '\n';
                }
                ++result.vertices;
            }

            for (int t = 1; t <= tri->NbTriangles(); ++t) {
                int a = 0, b = 0, c = 0;
                tri->Triangle(t).Get(a, b, c);
                // Keep the outward winding: reversed faces have their
                // triangulation wound the other way.
                if (reversed)
                    std::swap(b, c);
                const int ia = vertexOffset + a;
                const int ib = vertexOffset + b;
                const int ic = vertexOffset + c;
                if (hasNormals) {
                    out << "f " << ia << "//" << ia << ' ' << ib << "//" << ib
                        << ' ' << ic << "//" << ic << '\n';
                } else {
                    out << "f " << ia << ' ' << ib << ' ' << ic << '\n';
                }
                ++result.faces;
            }

            vertexOffset += nbNodes;
        }

        ++result.solids;
    }

    out.flush();
    file.close();

    result.ok = true;
    return result;
}

} // namespace viki
