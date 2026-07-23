#pragma once

#include <optional>

#include "EntitiesEx.h"

namespace viki {

// Rectangle recognition on a PolylineEntity, for Length/Height editing in
// the Properties panel (usage fix 2026-07-23: "les propriétés d'un rectangle
// ne permettent pas d'éditer ses dimensions").
//
// A polyline IS a rectangle when it is CLOSED, has exactly 4 vertices, no
// bulges (straight sides only), non-degenerate sides and square corners
// (|cos| of every corner angle <= 1e-6) — in ANY orientation: a rectangle
// rotated 30° still qualifies and KEEPS its own axes when edited.
//
// Editing convention (documented choice):
//  - the ANCHOR is the vertex closest to the world origin (ties -> lowest
//    vertex index). It never moves: deterministic, and for the common
//    RECT-from-lower-left case it is the corner the user drew first.
//  - Length is the LONGER side, Height the shorter (ties -> the side that
//    leaves the anchor toward the next vertex is the Length). Edits stretch
//    each dimension along its ORIGINAL axis, so a rotated rectangle stays
//    rotated and L stays along its original long axis.
struct RectProfile {
    int anchorIndex = 0; // vertex kept fixed on edits
    Vec2d anchor;
    Vec2d dirFwd;        // unit: anchor -> next vertex (cyclic)
    Vec2d dirBack;       // unit: anchor -> previous vertex
    double sideFwd = 0.0;
    double sideBack = 0.0;
    bool longIsFwd = true; // Length lies along dirFwd

    double length() const { return longIsFwd ? sideFwd : sideBack; }
    double height() const { return longIsFwd ? sideBack : sideFwd; }
    double area() const { return sideFwd * sideBack; }
};

// Recognize `pl` as a rectangle; nullopt when it is not one.
std::optional<RectProfile> rectProfileOf(const PolylineEntity& pl);

// Rebuild the polyline's vertices for the new Length x Height (both > 0),
// anchor and axes preserved, vertex order/indices preserved. Returns false
// (and leaves `pl` untouched) when the polyline is not a rectangle or a
// dimension is not positive. Caller wraps this in a transaction.
bool applyRectDims(PolylineEntity& pl, double newLength, double newHeight);

} // namespace viki
