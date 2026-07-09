#pragma once

#include <optional>
#include <vector>

#include <TopoDS_Shape.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include "doc/Document.h"

namespace viki {

// Work plane for sketching/extruding: an orthonormal frame. A 2D sketch point
// (u,v) maps to origin + u*xDir + v*(normal x xDir); extrusion runs along the
// normal. Default = the world XY plane, so 2D drafting is unchanged.
struct WorkPlane {
    gp_Pnt origin = gp_Pnt(0, 0, 0);
    gp_Dir normal = gp_Dir(0, 0, 1);
    gp_Dir xDir = gp_Dir(1, 0, 0);
};

namespace solidops {

struct WireResult {
    bool ok = false;
    QString message;
    std::vector<TopoDS_Wire> wires; // closed profile wires on the work plane
};

// Builds closed profile wires from 2D entities: circles, closed polylines,
// full ellipses, and chains of lines/arcs that close up.
WireResult wiresFromEntities(const Document& doc, const std::vector<EntityId>& ids,
                             const WorkPlane& plane);

struct SolidResult {
    bool ok = false;
    QString message;
    TopoDS_Shape shape;
};

// Prism along the work-plane normal (negative height extrudes the other way).
SolidResult extrudeWires(const std::vector<TopoDS_Wire>& wires, double height,
                         const WorkPlane& plane = {});

// EXTRUDE modes. NewBody = a standalone prism (default, same as extrudeWires).
// Join = fuse the prism onto `target`. Cut = subtract the prism from `target`.
// Symmetric = extrude `height/2` on both sides of the work plane (total height
// stays `height`, centred on the plane) as a new body.
enum class ExtrudeMode { NewBody, Join, Cut, Symmetric };

// Extrude with a mode. For NewBody/Symmetric `target` is ignored (pass a null
// shape). For Join/Cut `target` is the existing solid to fuse/subtract against;
// the result is `target` ± the freshly extruded prism.
SolidResult extrudeWires(const std::vector<TopoDS_Wire>& wires, double height,
                         const WorkPlane& plane, ExtrudeMode mode,
                         const TopoDS_Shape& target = {});

// Revolution around the axis through a->b (in the work plane), angle radians.
SolidResult revolveWires(const std::vector<TopoDS_Wire>& wires, const Vec2d& axisA,
                         const Vec2d& axisB, double angle, const WorkPlane& plane);

enum class BoolOp { Union, Subtract, Intersect };
SolidResult booleanOp(const TopoDS_Shape& a, const TopoDS_Shape& b, BoolOp op);

// Parametric HOLE: drill a cylinder of `diameter` at the 2D `center` on the
// work plane, boring along the plane normal, and Cut it from `solid`. With
// `through = true` the bore pierces the whole solid (depth is ignored and the
// cylinder spans the full extent of `solid` along the normal, with margin);
// otherwise it goes `depth` deep from the work plane into the material
// (opposite the normal). Result volume is the solid minus the removed cylinder.
SolidResult makeHole(const TopoDS_Shape& solid, const WorkPlane& plane,
                     const Vec2d& center, double diameter, double depth,
                     bool through);

// FILLET/CHAMFER on SPECIFIC edges (not all). `edges` are edges OF `solid`
// (TopoDS_Edge, as picked in the 3D view). filletEdges rounds each by `radius`;
// chamferEdges bevels each by `distance`. Empty `edges` is a no-op failure.
// Uses BRepFilletAPI_Make{Fillet,Chamfer}; OCCT throws on infeasible sizes, so
// force .Shape() and null-check rather than trusting IsDone().
SolidResult filletEdges(const TopoDS_Shape& solid,
                        const std::vector<TopoDS_Shape>& edges, double radius);
SolidResult chamferEdges(const TopoDS_Shape& solid,
                         const std::vector<TopoDS_Shape>& edges, double distance);

// Headless-driveable variants for tests/scripting: fillet/chamfer the first `n`
// edges of `solid` in TopExp exploration order (n <= 0 or beyond the edge count
// takes all edges). No GUI edge-picking needed.
SolidResult filletFirstNEdges(const TopoDS_Shape& solid, int n, double radius);
SolidResult chamferFirstNEdges(const TopoDS_Shape& solid, int n, double distance);

// SHELL: hollow `solid` out to a wall of `thickness`, leaving a shell. With an
// `openFace` (a face OF `solid`) that face is removed so the shell is open on
// that side; otherwise the shell is closed all around (an empty box). Positive
// thickness hollows inward. Uses BRepOffsetAPI_MakeThickSolid.
SolidResult shellSolid(const TopoDS_Shape& solid, double thickness,
                       const TopoDS_Shape& openFace = {});

// Push/Pull: extrude `face` (a face OF `solid`) along its outward normal by
// `distance`, then fuse (distance > 0, a boss) or cut (distance < 0, a pocket)
// with the solid. This is the direct-modeling "extrude a face" operation.
SolidResult pushPullFace(const TopoDS_Shape& solid, const TopoDS_Shape& face,
                         double distance);

// The sketch plane of a PLANAR face (for "sketch on this face"). Null for
// curved faces.
std::optional<WorkPlane> planeFromFace(const TopoDS_Shape& face);

// Assembly MATE constraint between two PLANAR faces. Returns the gp_Trsf that
// rigidly moves the solid owning `faceA` so that `faceA` becomes coincident
// with `faceB` (their planes overlap) and their outward normals are opposed
// (the two parts sit face-to-face, like two magnets snapping flat). `faceA`
// belongs to the moving solid; `faceB` to the fixed solid. std::nullopt if
// either face is missing or non-planar. Apply with SolidEntity::applyTrsf.
std::optional<gp_Trsf> mateTransform(const TopoDS_Shape& faceA,
                                     const TopoDS_Shape& faceB);

// Minimum 3D distance between two shapes (solids, faces, edges…), in mm, using
// BRepExtrema_DistShapeShape. Returns 0 when the shapes touch or interpenetrate.
// Returns -1 on failure (null shapes or the extrema solver not done).
double minDistance(const TopoDS_Shape& a, const TopoDS_Shape& b);

// The face's boundary edges projected into the work-plane's 2D (u,v) frame —
// reference geometry drawn in the 2D canvas so you sketch on the real face
// outline (holes included), not a blank rectangle. One polyline per edge.
std::vector<std::vector<Vec2d>> faceOutline2d(const TopoDS_Shape& face,
                                              const WorkPlane& plane);

// Object-snap targets on the face outline, in the work-plane's 2D frame, so a
// sketch profile can snap to real face features. Emits an Endpoint at every
// edge vertex and a Center at every circular/elliptical edge's center. Feed the
// result into Document::setExtraSnapPoints while sketching on the face.
std::vector<SnapPoint> faceSnapPoints2d(const TopoDS_Shape& face,
                                        const WorkPlane& plane);

} // namespace solidops

// The current work plane, per document (small registry, v1). Set by WORKPLANE
// and by "sketch on face"; read by EXTRUDE/REVOLVE.
WorkPlane& documentWorkplane(const Document& doc);
} // namespace viki
