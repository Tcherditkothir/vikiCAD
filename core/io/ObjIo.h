#pragma once

#include <QString>

#include "doc/Document.h"

namespace viki {

struct ObjResult {
    bool ok = false;
    QString error;
    int solids = 0;    // number of SolidEntity meshed into the OBJ
    int vertices = 0;  // total v lines written
    int faces = 0;     // total f (triangle) lines written
};

// Wavefront OBJ export for 3D printing / interchange. Every SolidEntity is
// meshed with BRepMesh_IncrementalMesh at the given linear deflection (mm),
// then written as an ASCII OBJ with `v` vertex lines, `vn` normals and `f`
// triangular faces. Self-contained (no third-party writer): the triangulation
// is walked directly from the meshed shape.
ObjResult exportObj(const Document& doc, const QString& path,
                    double deflection = 0.1);

} // namespace viki
