#pragma once

#include <QStringList>

#include "doc/Document.h"

// Minimum EDGE-TO-EDGE distance between two entities, material semantics —
// the CAM clearance measurement:
//   - wide polyline (Gerber trace)  = its round-capped stroke footprint
//     (centerline distance minus width/2 on each side);
//   - circle (drill hit, full pad)  = a filled disk of its radius;
//   - insert (flashed GBR-* pad)    = the block's REAL solid footprint
//     (the aperture polygons), transformed by the insert;
//   - hatch (region/pour)           = its filled rings (even-odd);
//   - anything else that strokes    = its flattened outline;
//   - anything that cannot be reduced to strokes (text...) falls back to its
//     BOUNDING BOX and the result is flagged approximate (`exact = false`).
//
// Overlapping/touching material reports distance 0 with `overlap = true`.

namespace viki {
namespace measure {

struct MinDistResult {
    bool ok = false;
    QString error;
    double distance = 0.0; // mm, edge to edge; 0 when overlapping
    bool overlap = false;
    Vec2d pa;              // closest point on entity A's material edge
    Vec2d pb;              // closest point on entity B's material edge
    bool exact = true;     // false: at least one side measured on its bbox
    QStringList notes;     // honesty notes ("#12 measured on bounding box")
};

MinDistResult minDistance(const Document& doc, EntityId a, EntityId b);

} // namespace measure
} // namespace viki
