#pragma once

#include <cstdint>
#include <vector>

#include <QString>

#include "geom/BBox2d.h"
#include "geom/Vec2d.h"

namespace viki {

// Device-agnostic render output. Entities flatten themselves into these;
// the canvas (QPainter), the PDF plotter and hit-testing all consume them.
struct StrokePrimitive {
    std::vector<Vec2d> points;   // world coordinates, mm
    uint32_t rgb = 0xFFFFFF;     // resolved color (ByLayer already applied)
    bool closed = false;
    bool filled = false;         // closed + filled (arrowheads, solid hatch)
    // Stroke thickness in world mm; 0 = cosmetic hairline (the default).
    // Renderers draw wide strokes with ROUND caps and joins — the exact
    // footprint of a Gerber round-aperture draw.
    double width = 0.0;
};

enum class TextHAlign { Left, Center, Right };
// Vertical anchor of a text ENTITY (primitives stay baseline-anchored; the
// entity offsets its baselines). Baseline = legacy TEXT default.
enum class TextVAlign { Baseline, Top, Middle, Bottom };

struct TextPrimitive {
    Vec2d pos;                   // baseline-left anchor (world)
    double height = 3.5;         // cap height in world units
    double rotation = 0.0;       // radians CCW
    QString text;                // single line (entities split multiline)
    uint32_t rgb = 0xFFFFFF;
    TextHAlign hAlign = TextHAlign::Left;
};

struct PrimitiveList {
    std::vector<StrokePrimitive> strokes;
    std::vector<TextPrimitive> texts;
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
    // Document access for entities that need styles/units to regenerate
    // (dimensions). Null = built-in defaults.
    const class Document* doc = nullptr;
    // Set by hit-testing so text entities emit their (invisible) pick box.
    bool forHitTest = false;
};

} // namespace viki
