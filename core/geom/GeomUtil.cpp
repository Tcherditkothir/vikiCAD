#include "GeomUtil.h"

#include <algorithm>

namespace viki {

std::optional<CircleFrom3> circleFrom3Points(const Vec2d& p1, const Vec2d& p2, const Vec2d& p3)
{
    const Vec2d d12 = p2 - p1;
    const Vec2d d13 = p3 - p1;
    const double cross = d12.cross(d13);
    // Collinearity test scaled to the point spread so it behaves the same at
    // any drawing size.
    const double scale = std::max(d12.lengthSq(), d13.lengthSq());
    if (std::fabs(cross) <= 1e-12 * scale)
        return std::nullopt;

    // Intersection of the two chord bisectors, solved directly.
    const double sq12 = d12.lengthSq();
    const double sq13 = d13.lengthSq();
    const double inv = 0.5 / cross;
    const Vec2d center{
        p1.x + inv * (d13.y * sq12 - d12.y * sq13),
        p1.y + inv * (d12.x * sq13 - d13.x * sq12)};
    return CircleFrom3{center, center.distanceTo(p1)};
}

double distanceToSegment(const Vec2d& p, const Vec2d& a, const Vec2d& b)
{
    const Vec2d ab = b - a;
    const double lenSq = ab.lengthSq();
    if (nearZero(lenSq))
        return p.distanceTo(a);
    const double t = std::clamp((p - a).dot(ab) / lenSq, 0.0, 1.0);
    return p.distanceTo(a + ab * t);
}

double normalizeAngle(double radians)
{
    const double twoPi = 2.0 * M_PI;
    double r = std::fmod(radians, twoPi);
    if (r < 0)
        r += twoPi;
    return r;
}

double ccwSweep(double startAngle, double endAngle)
{
    const double twoPi = 2.0 * M_PI;
    double sweep = normalizeAngle(endAngle) - normalizeAngle(startAngle);
    if (sweep <= 0)
        sweep += twoPi;
    return sweep;
}

bool angleOnArc(double angle, double startAngle, double sweep)
{
    const double rel = normalizeAngle(angle - startAngle);
    return rel <= sweep + kGeomTol;
}

} // namespace viki
