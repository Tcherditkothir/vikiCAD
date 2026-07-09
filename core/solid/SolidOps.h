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

// The face's boundary edges projected into the work-plane's 2D (u,v) frame —
// reference geometry drawn in the 2D canvas so you sketch on the real face
// outline (holes included), not a blank rectangle. One polyline per edge.
std::vector<std::vector<Vec2d>> faceOutline2d(const TopoDS_Shape& face,
                                              const WorkPlane& plane);

} // namespace solidops

// The current work plane, per document (small registry, v1). Set by WORKPLANE
// and by "sketch on face"; read by EXTRUDE/REVOLVE.
WorkPlane& documentWorkplane(const Document& doc);
} // namespace viki
