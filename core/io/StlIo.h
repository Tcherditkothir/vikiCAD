#pragma once

#include <memory>

#include <QString>

#include <TopoDS_Shape.hxx>

#include "doc/Document.h"

namespace viki {

struct StlResult {
    bool ok = false;
    QString error;
    int solids = 0;    // number of SolidEntity meshed into the STL
    int triangles = 0; // facets read (import) — 0 on export
    // Import advisories (issue #4): an STL lands as a MESH; STEP export of a
    // mesh-backed solid writes an empty MANIFOLD_SURFACE_SHAPE_REPRESENTATION
    // (geometry silently lost). The cure is MESH2SOLID first — warn the caller
    // right at import, both CLI JSON and GUI history.
    QStringList warnings;
};

// STL export for 3D printing. Every SolidEntity is meshed with
// BRepMesh_IncrementalMesh at the given linear deflection (mm), then written
// with StlAPI_Writer. `ascii = false` writes a compact binary STL (the default
// for slicers); `ascii = true` writes a human-readable ASCII STL.
StlResult exportStl(const Document& doc, const QString& path,
                    double deflection = 0.1, bool ascii = false);

// STL import. Both dialects (ASCII and binary) are detected by content, not by
// extension. The triangulation lands in ONE TopoDS_Face that carries no
// geometric surface — an OCCT shape whose only content is its mesh. That face
// goes into a SolidEntity, so an imported mesh rides every existing mechanism
// for free: BinTools serialization (`.vkd`), the undo journal, MOVE3D/ROTATE3D,
// the 3D view, STL/OBJ re-export.
//
// Deliberately NOT sewn into a BREP solid: STL carries no topology, so sewing
// costs one planar face per triangle. MESH2SOLID does that on demand — see
// meshToSolid() — but a mesh is the right default for the usual job of dropping
// an off-the-shelf part next to your own design.
StlResult importStl(const QString& path, std::unique_ptr<Document>& outDoc);

// True when `shape` is a mesh-backed face as produced by importStl (a face with
// a triangulation but no surface). Callers that need real geometry — booleans,
// fillets, push/pull — use this to refuse early with a clear message instead of
// failing deep inside OCCT.
bool isMeshShape(const TopoDS_Shape& shape);

// Number of triangles in a mesh-backed shape (0 when it carries none).
int meshTriangleCount(const TopoDS_Shape& shape);

struct MeshToSolidResult {
    bool ok = false;
    QString error;        // set only when ok is false
    QString warning;      // advisory on a successful conversion (skipped facets)
    TopoDS_Shape shape;
    int triangles = 0;    // facets fed to the sewer
    int faces = 0;        // faces in `shape`
    // Issue #8: a mesh does NOT have to be one body. A concatenated union (two
    // overlapping shells sharing no vertex, exactly what a naive trimesh export
    // produces) sews into several disjoint closed shells. The old single-shell
    // code path dropped those cases to "open shell" and never made a solid of
    // them. So the sewn shape is now walked shell by shell.
    int solids = 0;       // closed shells that became real TopAbs_SOLID bodies
    int openShells = 0;   // shells left as shells: genuinely open ones, plus
                          // closed-but-unbuildable ones. Never dropped.
    int reoriented = 0;   // closed shells whose faces pointed INWARD and were
                          // flipped so the signed volume comes out positive
    bool closed = false;  // true when EVERY shell came out a watertight solid
};

// Default cap on MESH2SOLID input. Sewing costs one planar face per triangle
// and the cost grows faster than the triangle count. Measured here on real
// downloaded parts (debug build, so a release build is several times quicker):
//
//      4 014 triangles ->  0.7 s
//     11 140 triangles ->  2.9 s
//     52 484 triangles -> 39.3 s,  817 MB peak
//    107 628 triangles -> 48.3 s, 1.64 GB peak
//
// Memory is the real wall, not time. 20k lands around ten seconds and a few
// hundred megabytes; past that the sensible answer is usually "keep it a mesh",
// since a 100k-FACE solid makes every later operation glacial anyway. The
// MESH2SOLID command reads VIKICAD_MESH2SOLID_MAX to override this.
inline constexpr int kMeshToSolidMaxTriangles = 20000;

// Convert a mesh-backed shape (see importStl) into real BREP geometry: every
// triangle becomes a planar face, the faces are sewn into shells, and each
// CLOSED shell becomes a solid. That is what unlocks booleans, sections and
// MINDIST on an imported STL.
//
// The result is faceted by construction -- a fillet on a facet edge has no
// geometric meaning -- so this is a deliberate, explicit step (the MESH2SOLID
// command), never something the importer does behind your back.
//
// Signed volume is normalised (issue #8): sewing preserves whatever winding
// came in, so a mesh exported with inward-facing triangles sews into a
// watertight shell whose volume is NEGATIVE, and every downstream consumer
// (measure, mass, STEP, booleans) then sees anti-material. Sewing has no
// opinion about which side is "outside" -- the surface alone cannot say -- so
// each closed shell is checked and flipped when its signed volume is negative,
// which is the deterministic, cheap fix and matches what trimesh's
// `repair.fix_normals` does for the caller.
//
// Never reports success with a null shape: an OCCT sewing pass can "succeed"
// and hand back nothing (see LESSONS, the requireSolid rule).
MeshToSolidResult meshToSolid(const TopoDS_Shape& mesh,
                              int maxTriangles = kMeshToSolidMaxTriangles);

} // namespace viki
