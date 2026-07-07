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

// Normalize an angle into [0, 2*pi).
double normalizeAngle(double radians);

// CCW sweep from startAngle to endAngle, in (0, 2*pi].
double ccwSweep(double startAngle, double endAngle);

// True if `angle` lies on the CCW arc from startAngle spanning `sweep`.
bool angleOnArc(double angle, double startAngle, double sweep);

} // namespace viki
