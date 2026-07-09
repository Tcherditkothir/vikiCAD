#pragma once

#include <vector>

#include "Vec2d.h"

namespace viki {

// Planar-arrangement / face-finding for a set of intersecting 2D curves.
//
// Fusion-style "auto region" detection: given a soup of curves (lines, arcs,
// circles) that cross each other, find the minimal closed regions they bound
// instead of requiring one pre-closed loop.
//
// Curves are supplied as an abstract set of `Curve`s; arcs and circles are
// polyline-approximated internally at `chordTolerance`. The algorithm splits
// every curve at all pairwise intersections, welds coincident vertices into a
// planar graph, and walks minimal faces (most-clockwise-turn traversal). The
// unbounded outer face is discarded, so what remains are exactly the bounded
// closed regions.

// One input curve. A line segment is (a -> b). A circle is
// (isArc && sweep >= 2pi). An arc is a CCW arc [startAngle, startAngle+sweep].
struct Curve {
    bool isArc = false;
    // Line segment endpoints (used when !isArc).
    Vec2d a;
    Vec2d b;
    // Arc / circle (used when isArc).
    Vec2d center;
    double radius = 0.0;
    double startAngle = 0.0; // radians
    double sweep = 0.0;      // radians, > 0

    static Curve line(const Vec2d& a, const Vec2d& b)
    {
        Curve c;
        c.isArc = false;
        c.a = a;
        c.b = b;
        return c;
    }
    static Curve arc(const Vec2d& center, double radius, double startAngle, double sweep)
    {
        Curve c;
        c.isArc = true;
        c.center = center;
        c.radius = radius;
        c.startAngle = startAngle;
        c.sweep = sweep;
        return c;
    }
    static Curve circle(const Vec2d& center, double radius)
    {
        return arc(center, radius, 0.0, 2.0 * M_PI);
    }
};

// A detected closed region: a CCW-ordered ring of world points and its
// (positive) area.
struct Region {
    std::vector<Vec2d> boundary; // CCW, does not repeat the first point
    double area = 0.0;           // signed area, always > 0 (CCW)
};

// Find the minimal bounded regions enclosed by the arrangement of `curves`.
// `chordTolerance` controls arc flattening (mm); smaller = finer. `weldTol`
// welds vertices closer than this (mm) into one node — set a hair above the
// arc chord error so shared endpoints coincide.
std::vector<Region> findRegions(const std::vector<Curve>& curves,
                                double chordTolerance = 0.05,
                                double weldTol = 1e-6);

} // namespace viki
