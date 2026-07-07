#pragma once

#include <cstdint>
#include <vector>

#include "geom/BBox2d.h"
#include "geom/Vec2d.h"

namespace viki {

// Device-agnostic render output. Entities flatten themselves into these;
// the canvas (QPainter), the PDF plotter and hit-testing all consume them.
struct StrokePrimitive {
    std::vector<Vec2d> points;   // world coordinates, mm
    uint32_t rgb = 0xFFFFFF;     // resolved color (ByLayer already applied)
    bool closed = false;
};

struct PrimitiveList {
    std::vector<StrokePrimitive> strokes;
};

struct RenderContext {
    // Max chord deviation for curve flattening, in world units (mm).
    // Callers derive it from the current zoom (≈0.25 px worth of mm).
    double chordTolerance = 0.01;
    uint32_t resolvedColor = 0xFFFFFF;
    // World size of one pixel — for screen-constant glyphs (point markers).
    double pixelSize = 0.25;
    // Visible world region; infinite entities (xline) clip themselves to it.
    // Invalid box = unbounded context (fallback clip at ±1e6 mm).
    BBox2d viewBox;
};

} // namespace viki
