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
    int materials = 0; // distinct materials written to the .mtl sidecar
    QString mtlPath;   // the .mtl written, empty when the model has no colour
};

// Wavefront OBJ export for 3D printing / interchange. Every SolidEntity is
// meshed with BRepMesh_IncrementalMesh at the given linear deflection (mm),
// then written as an ASCII OBJ with `v` vertex lines, `vn` normals and `f`
// triangular faces. Self-contained (no third-party writer): the triangulation
// is walked directly from the meshed shape.
//
// Colour travels in the companion `.mtl` beside the `.obj` (OBJ itself has no
// notion of colour): one material per distinct colour+transparency pair,
// referenced by `usemtl` before each solid's faces. A model whose solids are all
// ByLayer gets no `.mtl` and no `mtllib` line — an empty material library would
// only make readers warn.
ObjResult exportObj(const Document& doc, const QString& path,
                    double deflection = 0.1);

} // namespace viki
