#include "StlIo.h"

#include <BRep_Builder.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <StlAPI_Writer.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shape.hxx>

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

} // namespace viki
