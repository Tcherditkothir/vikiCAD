#pragma once

#include <vector>

#include <TopoDS_Shape.hxx>
#include <gp_Dir.hxx>

#include "geom/Vec2d.h"

namespace viki {

// Hidden Line Removal (HLR) projection of a 3D shape onto a 2D drawing plane —
// the geometric core of a "mise en plan" / standard orthographic view.
//
// The camera looks ALONG `viewDir` (eye -> target). The projection plane is the
// plane perpendicular to viewDir; 2D coordinates are expressed in an in-plane
// frame (right, up). Coordinates are in mm, matching the model.
//
// A DrawingSegment is a straight chord; curved edges (circles, splines) are
// discretized into a chain of chords so downstream 2D consumers only deal with
// line segments.
struct DrawingSegment {
    Vec2d a;
    Vec2d b;
};

struct DrawingProjection {
    // Sharp visible edges of the projection (the outline + visible internal
    // edges), as 2D segments in the view plane.
    std::vector<DrawingSegment> visible;
    // Hidden edges (occluded), kept separate so a caller can draw them dashed
    // or drop them entirely.
    std::vector<DrawingSegment> hidden;
};

namespace render {

// Project `shape` to a 2D drawing via HLR along `viewDir` (the direction the
// camera looks). `deflection` bounds the chord error when discretizing curved
// projected edges (mm). Returns empty vectors if HLR fails or the shape is null.
DrawingProjection projectToDrawing(const TopoDS_Shape& shape, const gp_Dir& viewDir,
                                   double deflection = 0.05);

} // namespace render

} // namespace viki
