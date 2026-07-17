#include "GeomUtil.h"

#include <algorithm>
#include <limits>

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

Vec2d closestPointOnSegment(const Vec2d& p, const Vec2d& a, const Vec2d& b)
{
    const Vec2d ab = b - a;
    const double lenSq = ab.lengthSq();
    if (nearZero(lenSq))
        return a;
    const double t = std::clamp((p - a).dot(ab) / lenSq, 0.0, 1.0);
    return a + ab * t;
}

double closestSegSeg(const Vec2d& a1, const Vec2d& a2, const Vec2d& b1, const Vec2d& b2,
                     Vec2d& p, Vec2d& q)
{
    // Proper crossing = distance zero at the intersection point.
    const Vec2d r = a2 - a1;
    const Vec2d s = b2 - b1;
    const double denom = r.cross(s);
    if (!nearZero(denom, 1e-14)) {
        const double t = (b1 - a1).cross(s) / denom;
        const double u = (b1 - a1).cross(r) / denom;
        if (t >= 0.0 && t <= 1.0 && u >= 0.0 && u <= 1.0) {
            p = q = a1 + r * t;
            return 0.0;
        }
    }
    // No crossing: the minimum is endpoint-to-segment; try the 4 candidates.
    double best = std::numeric_limits<double>::max();
    const auto consider = [&](const Vec2d& onA, const Vec2d& onB) {
        const double d = onA.distanceTo(onB);
        if (d < best) {
            best = d;
            p = onA;
            q = onB;
        }
    };
    consider(a1, closestPointOnSegment(a1, b1, b2));
    consider(a2, closestPointOnSegment(a2, b1, b2));
    consider(closestPointOnSegment(b1, a1, a2), b1);
    consider(closestPointOnSegment(b2, a1, a2), b2);
    return best;
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
