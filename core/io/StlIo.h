#pragma once

#include <QString>

#include "doc/Document.h"

namespace viki {

struct StlResult {
    bool ok = false;
    QString error;
    int solids = 0; // number of SolidEntity meshed into the STL
};

// STL export for 3D printing. Every SolidEntity is meshed with
// BRepMesh_IncrementalMesh at the given linear deflection (mm), then written
// with StlAPI_Writer. `ascii = false` writes a compact binary STL (the default
// for slicers); `ascii = true` writes a human-readable ASCII STL.
StlResult exportStl(const Document& doc, const QString& path,
                    double deflection = 0.1, bool ascii = false);

} // namespace viki
