#pragma once

#include <optional>
#include <vector>

#include <TopoDS_Shape.hxx>
#include <TopoDS_Wire.hxx>

#include "doc/Document.h"

namespace viki {

// Work plane for sketching/extruding. v1: the XY plane at an optional Z
// offset (face-picking planes arrive with the 3D GUI work).
struct WorkPlane {
    double zOffset = 0.0;
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

// Prism along +Z (negative height extrudes downward).
SolidResult extrudeWires(const std::vector<TopoDS_Wire>& wires, double height);

// Revolution around the axis through a->b (in the work plane), angle radians.
SolidResult revolveWires(const std::vector<TopoDS_Wire>& wires, const Vec2d& axisA,
                         const Vec2d& axisB, double angle, const WorkPlane& plane);

enum class BoolOp { Union, Subtract, Intersect };
SolidResult booleanOp(const TopoDS_Shape& a, const TopoDS_Shape& b, BoolOp op);

} // namespace solidops
} // namespace viki
