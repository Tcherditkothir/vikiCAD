#pragma once

#include <vector>

#include "Vec2d.h"

namespace viki {

// Analytic intersections between line segments and circular arcs.
// All functions append world-space intersection points to `out`.

// Infinite-line X infinite-line (single point unless parallel).
void intersectLinesInf(const Vec2d& a1, const Vec2d& a2, const Vec2d& b1, const Vec2d& b2,
                       std::vector<Vec2d>& out);

// Segment X segment (bounded both sides).
void intersectSegSeg(const Vec2d& a1, const Vec2d& a2, const Vec2d& b1, const Vec2d& b2,
                     std::vector<Vec2d>& out);

// Infinite line X full circle.
void intersectLineCircle(const Vec2d& a1, const Vec2d& a2, const Vec2d& center, double r,
                         std::vector<Vec2d>& out);

// Segment X arc (bounded segment, bounded CCW arc [a0, a0+sweep]).
void intersectSegArc(const Vec2d& a1, const Vec2d& a2, const Vec2d& center, double r,
                     double a0, double sweep, std::vector<Vec2d>& out);

// Full circle X full circle.
void intersectCircleCircle(const Vec2d& c1, double r1, const Vec2d& c2, double r2,
                           std::vector<Vec2d>& out);

// Arc X arc (both bounded CCW).
void intersectArcArc(const Vec2d& c1, double r1, double s1, double w1, const Vec2d& c2,
                     double r2, double s2, double w2, std::vector<Vec2d>& out);

} // namespace viki
