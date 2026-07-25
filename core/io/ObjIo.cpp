#include "ObjIo.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <QFile>
#include <QFileInfo>
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
namespace {

// Material names must be stable and free of spaces (MTL is whitespace-split).
QString materialName(int index)
{
    return QStringLiteral("vikicad_%1").arg(index);
}

} // namespace

ObjResult exportObj(const Document& doc, const QString& path, double deflection)
{
    ObjResult result;

    if (deflection <= 0.0)
        deflection = 0.1;

    // Collect the solids up front so we can fail cleanly on an empty model
    // before touching the output file. Each keeps its appearance, which becomes
    // a material below.
    struct Part {
        TopoDS_Shape shape;
        bool hasColor = false;
        uint32_t rgb = 0xFFFFFF;
        double transparency = 0.0;
        QString component;
    };
    std::vector<Part> parts;
    for (const EntityId id : doc.drawOrder()) {
        const auto* solid = dynamic_cast<const SolidEntity*>(doc.entity(id));
        if (!solid || solid->shape().IsNull())
            continue;
        Part p;
        p.shape = solid->shape();
        p.hasColor = !solid->color().byLayer;
        p.rgb = solid->color().rgb;
        p.transparency = std::clamp(solid->transparency, 0.0, 1.0);
        p.component = solid->component;
        parts.push_back(std::move(p));
    }

    if (parts.empty()) {
        result.error =
            QStringLiteral("no solids to export (EXTRUDE/REVOLVE first)");
        return result;
    }

    // Distinct colour+transparency pairs, in first-seen order so the .mtl reads
    // in the same order as the model.
    std::vector<std::pair<uint32_t, double>> palette;
    std::vector<int> partMaterial(parts.size(), -1);
    for (size_t i = 0; i < parts.size(); ++i) {
        if (!parts[i].hasColor)
            continue;
        const std::pair<uint32_t, double> key{parts[i].rgb, parts[i].transparency};
        auto it = std::find(palette.begin(), palette.end(), key);
        if (it == palette.end()) {
            palette.push_back(key);
            it = std::prev(palette.end());
        }
        partMaterial[i] = int(it - palette.begin());
    }

    // The .mtl sits beside the .obj and is referenced by base name, so moving
    // the pair together keeps it resolvable.
    QString mtlName;
    if (!palette.empty()) {
        const QFileInfo info(path);
        mtlName = info.completeBaseName() + QStringLiteral(".mtl");
        const QString mtlPath = info.absolutePath() + QLatin1Char('/') + mtlName;
        QFile mtl(mtlPath);
        if (!mtl.open(QIODevice::WriteOnly | QIODevice::Text)) {
            result.error = QStringLiteral("cannot write MTL to %1").arg(mtlPath);
            return result;
        }
        QTextStream ms(&mtl);
        ms << "# Material library exported by VikiCAD\n";
        for (size_t i = 0; i < palette.size(); ++i) {
            const auto [rgb, transparency] = palette[i];
            const double r = double((rgb >> 16) & 0xFF) / 255.0;
            const double g = double((rgb >> 8) & 0xFF) / 255.0;
            const double b = double(rgb & 0xFF) / 255.0;
            ms << "\nnewmtl " << materialName(int(i)) << '\n';
            ms << "Kd " << r << ' ' << g << ' ' << b << '\n';
            // `d` is opacity (1 = opaque). Tr is its complement; writing both
            // covers readers that only honour one of them.
            ms << "d " << (1.0 - transparency) << '\n';
            ms << "Tr " << transparency << '\n';
            ms << "illum 1\n";
        }
        ms.flush();
        mtl.close();
        result.materials = int(palette.size());
        result.mtlPath = mtlPath;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        result.error = QStringLiteral("cannot write OBJ to %1").arg(path);
        return result;
    }

    QTextStream out(&file);
    out << "# Wavefront OBJ exported by VikiCAD\n";
    out << "# units: mm\n";
    if (!mtlName.isEmpty())
        out << "mtllib " << mtlName << '\n';

    // OBJ face indices are 1-based and global across the whole file, so we keep
    // a running offset as each face contributes its own block of vertices.
    int vertexOffset = 0;

    for (size_t partIndex = 0; partIndex < parts.size(); ++partIndex) {
        const TopoDS_Shape& shape = parts[partIndex].shape;
        // `o` names the object so a viewer's outliner is readable; `usemtl`
        // binds the appearance for every face that follows.
        if (!parts[partIndex].component.isEmpty())
            out << "o " << parts[partIndex].component << '\n';
        if (partMaterial[partIndex] >= 0)
            out << "usemtl " << materialName(partMaterial[partIndex]) << '\n';
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
