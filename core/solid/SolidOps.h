#pragma once

#include <optional>
#include <vector>

#include <TopoDS_Shape.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
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

// Builds an OPEN path wire (a spine to sweep along) from 2D line/arc/polyline
// entities on the work plane. Unlike wiresFromEntities the chain need NOT close.
// The result has ok=false if the entities don't chain end-to-end into a single
// path. Used by SWEEP.
WireResult pathWireFromEntities(const Document& doc, const std::vector<EntityId>& ids,
                                const WorkPlane& plane);

// Prism along the work-plane normal (negative height extrudes the other way).
SolidResult extrudeWires(const std::vector<TopoDS_Wire>& wires, double height,
                         const WorkPlane& plane = {});

// SWEEP: sweep the closed `profileWires` along the open `pathWire` spine,
// producing a solid (BRepOffsetAPI_MakePipe over a face per profile, fused).
// The profile is used as-is (its position relative to the path start matters):
// place it at the path's starting point for a clean sweep. Volume of a circle
// of radius r swept along a straight length L is ~= pi*r^2*L.
SolidResult sweepProfile(const std::vector<TopoDS_Wire>& profileWires,
                         const TopoDS_Wire& pathWire);

// LOFT: skin a solid (or shell, when `solid=false`) through the ordered cross-
// section wires (BRepOffsetAPI_ThruSections). Needs 2+ sections. Each section is
// one closed wire; sections are usually on parallel work planes at different Z.
SolidResult loftProfiles(const std::vector<TopoDS_Wire>& sections, bool solid);

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

// SPLIT BODY (Fusion "Split Body"): cut `solid` with `tool` — a FACE (planar
// or curved), a SHELL, or another SOLID — and return the resulting pieces as
// separate solids (BRepAlgoAPI_Splitter, solid as argument / tool as tool,
// result compound exploded into its TopAbs_SOLID children). 0 or 1 piece
// means the tool missed the solid or the split failed; callers should report
// that instead of replacing anything.
std::vector<TopoDS_Shape> splitSolid(const TopoDS_Shape& solid,
                                     const TopoDS_Shape& tool);

// Convenience: split by the infinite plane `pln`, realized as a large bounded
// planar face spanning the solid's bounding box (with margin) so the cut
// always covers the whole body.
std::vector<TopoDS_Shape> splitByPlane(const TopoDS_Shape& solid,
                                       const gp_Pln& pln);

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

// DRAFT (taper) faces for moldability: tilt each face in `faces` (faces OF
// `solid`) by `angleDeg` about its intersection with `neutralPlane`, using
// `pullDir` as the mold-pull direction. Positive angle adds material away from
// the pull direction (the classic draft that lets a part release from a mold).
// Uses BRepOffsetAPI_DraftAngle; OCCT throws on infeasible angles, so force
// .Shape() and null-check rather than trusting IsDone().
SolidResult draftFaces(const TopoDS_Shape& solid,
                       const std::vector<TopoDS_Shape>& faces, const gp_Dir& pullDir,
                       const gp_Pln& neutralPlane, double angleDeg);

// Headless-driveable variant for tests/scripting: draft the four SIDE faces of
// an axis-aligned box (the faces whose outward normal is perpendicular to
// `pullDir`), tilting them by `angleDeg` about `neutralPlane`. No GUI face
// picking needed. Returns ok=false if `solid` has no such side faces.
SolidResult draftBoxSides(const TopoDS_Shape& solid, const gp_Dir& pullDir,
                          const gp_Pln& neutralPlane, double angleDeg);

// Push/Pull: extrude `face` (a face OF `solid`) along its outward normal by
// `distance`, then fuse (distance > 0, a boss) or cut (distance < 0, a pocket)
// with the solid. This is the direct-modeling "extrude a face" operation.
SolidResult pushPullFace(const TopoDS_Shape& solid, const TopoDS_Shape& face,
                         double distance);

// The sketch plane of a PLANAR face (for "sketch on this face"). Null for
// curved faces.
std::optional<WorkPlane> planeFromFace(const TopoDS_Shape& face);

// Express a 3D point in a work plane's 2D sketch coordinates (the inverse of
// the plane's own to-3D mapping): u = (p - origin)·xDir, v = (p - origin)·yDir
// with yDir = normal × xDir. Feeds 3D-view mouse positions into 2D commands.
Vec2d projectToPlane2d(const gp_Pnt& p, const WorkPlane& plane);
// And back: the 3D point of sketch coords `uv` on `plane`.
gp_Pnt planePoint3d(const Vec2d& uv, const WorkPlane& plane);

// Assembly MATE constraint between two PLANAR faces. Returns the gp_Trsf that
// rigidly moves the solid owning `faceA` so that `faceA` becomes coincident
// with `faceB` (their planes overlap) and their outward normals are opposed
// (the two parts sit face-to-face, like two magnets snapping flat). `faceA`
// belongs to the moving solid; `faceB` to the fixed solid. std::nullopt if
// either face is missing or non-planar. Apply with SolidEntity::applyTrsf.
std::optional<gp_Trsf> mateTransform(const TopoDS_Shape& faceA,
                                     const TopoDS_Shape& faceB);

// SECTION: cut `solid` by the infinite plane `pln` and return the section
// profile as a compound of the cut edges/wires (BRepAlgoAPI_Section, with the
// section curves built). Empty (a null-ish compound) when the plane misses the
// solid. The wires lie in `pln` and bound the exposed cross-section.
TopoDS_Shape sectionWires(const TopoDS_Shape& solid, const gp_Pln& pln);

// Area of the cross-section produced by cutting `solid` with `pln`, in mm². The
// section edges are collected into closed wires, faced (BRepBuilderAPI_MakeFace
// on the section plane), and measured with BRepGProp::SurfaceProperties.
// Returns 0 when the plane misses the solid or the section is not a closed area
// (e.g. a tangent touch), and 0 on failure.
double sectionArea(const TopoDS_Shape& solid, const gp_Pln& pln);

// Minimum 3D distance between two shapes (solids, faces, edges…), in mm, using
// BRepExtrema_DistShapeShape. Returns 0 when the shapes touch or interpenetrate.
// Returns -1 on failure (null shapes or the extrema solver not done).
double minDistance(const TopoDS_Shape& a, const TopoDS_Shape& b);

// Interference (clash) volume between two solids, in mm³: the volume of their
// BRepAlgoAPI_Common (the overlapping material). Returns 0 when the solids are
// disjoint or only touch on a face/edge (a common with zero volume), and 0 on
// failure (null shapes or the boolean not done). Positive only for a genuine
// interpenetration.
double interferenceVolume(const TopoDS_Shape& a, const TopoDS_Shape& b);

// One overlapping pair found by checkAllInterferences.
struct Interference {
    EntityId a = 0;
    EntityId b = 0;
    double volume = 0.0; // mm³ of overlapping material (> tolerance)
};

// Sweep every unordered pair of SolidEntity in the document and return those
// that interpenetrate (interferenceVolume above `minVolume`, default 1e-6 mm³
// to ignore numeric noise from merely-touching parts). Pairs are reported with
// a < b entity ids, ordered by decreasing overlap volume.
std::vector<Interference> checkAllInterferences(const Document& doc,
                                                double minVolume = 1e-6);

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

// 3D PATTERNS (Fusion "Rectangular/Circular Pattern"): return the list of
// gp_Trsf placements to clone a picked solid into a grid or ring. The
// placements are returned (not the shapes) so the caller creates one new
// SolidEntity per transform, mirroring CommandsAssembly's gp_Trsf usage.
//
// patternRect: an nx*ny*nz grid stepping `dx`,`dy`,`dz` mm along the world
// X/Y/Z axes. The first cell (i=j=k=0) is the identity, so the original
// position is included. Counts below 1 are clamped to 1; the result has
// nx*ny*nz elements.
std::vector<gp_Trsf> patternRect(int nx, int ny, int nz, double dx, double dy,
                                 double dz);

// patternPolar: `count` copies spread over `totalAngleDeg` degrees about the
// axis through `center` along `axis` (a unit direction, typically world Z).
// The first copy is the identity (angle 0). When `totalAngleDeg` is 360 the
// copies are spaced by 360/count (the endpoint is not duplicated); otherwise
// they are evenly spaced across the arc, endpoints included (count>1).
// `count` below 1 is clamped to 1; the result has `count` elements.
std::vector<gp_Trsf> patternPolar(int count, const gp_Dir& axis,
                                  const gp_Pnt& center, double totalAngleDeg);

} // namespace solidops

// The current work plane, per document (small registry, v1). Set by WORKPLANE
// and by "sketch on face"; read by EXTRUDE/REVOLVE.
WorkPlane& documentWorkplane(const Document& doc);
} // namespace viki
