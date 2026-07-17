#pragma once

#include <optional>

#include "Vec2d.h"

namespace viki {

struct CircleFrom3 {
    Vec2d center;
    double radius;
};

// Circle through three points; nullopt when collinear.
std::optional<CircleFrom3> circleFrom3Points(const Vec2d& p1, const Vec2d& p2, const Vec2d& p3);

// Distance from p to segment [a,b].
double distanceToSegment(const Vec2d& p, const Vec2d& a, const Vec2d& b);

// Closest point of segment [a,b] to p.
Vec2d closestPointOnSegment(const Vec2d& p, const Vec2d& a, const Vec2d& b);

// Closest points between segments [a1,a2] and [b1,b2]. Fills p (on A) and
// q (on B), returns their distance (0 when the segments intersect).
double closestSegSeg(const Vec2d& a1, const Vec2d& a2, const Vec2d& b1, const Vec2d& b2,
                     Vec2d& p, Vec2d& q);

// Normalize an angle into [0, 2*pi).
double normalizeAngle(double radians);

// CCW sweep from startAngle to endAngle, in (0, 2*pi].
double ccwSweep(double startAngle, double endAngle);

// True if `angle` lies on the CCW arc from startAngle spanning `sweep`.
bool angleOnArc(double angle, double startAngle, double sweep);

} // namespace viki
