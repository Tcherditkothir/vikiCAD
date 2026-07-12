#pragma once

#include <TopoDS_Shape.hxx>

namespace viki {
namespace subshape {

// Deterministic sub-shape addressing — THE shared vocabulary between headless
// agents and the GUI. Every face/edge of a solid gets a stable 0-based index:
// the first-visit order of TopExp_Explorer over the shape with duplicates
// removed (shared edges appear once), i.e. TopExp::MapShapes order. INSPECT
// prints these indices, index-driven commands consume them, and the 3D-view
// pick history reports them ("picked face 3 of solid #2").

int faceCount(const TopoDS_Shape& shape);
int edgeCount(const TopoDS_Shape& shape);

// The face/edge at `index` (0-based, TopExp order). Null shape out of range.
TopoDS_Shape faceAt(const TopoDS_Shape& shape, int index);
TopoDS_Shape edgeAt(const TopoDS_Shape& shape, int index);

// Reverse lookup: index of `face`/`edge` within `shape` (IsSame match,
// orientation ignored). -1 when the sub-shape is not part of the shape.
int faceIndexOf(const TopoDS_Shape& shape, const TopoDS_Shape& face);
int edgeIndexOf(const TopoDS_Shape& shape, const TopoDS_Shape& edge);

} // namespace subshape
} // namespace viki
