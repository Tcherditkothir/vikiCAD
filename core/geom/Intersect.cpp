#include "Intersect.h"

#include "GeomUtil.h"

namespace viki {

namespace {
// Bound tolerance for parameter checks — slightly loose so touching
// endpoints count as intersections (what a drafter expects from TRIM).
constexpr double kParamTol = 1e-9;
} // namespace

void intersectLinesInf(const Vec2d& a1, const Vec2d& a2, const Vec2d& b1, const Vec2d& b2,
                       std::vector<Vec2d>& out)
{
    const Vec2d r = a2 - a1;
    const Vec2d s = b2 - b1;
    const double denom = r.cross(s);
    if (nearZero(denom, 1e-14))
        return;
    const double t = (b1 - a1).cross(s) / denom;
    out.push_back(a1 + r * t);
}

void intersectSegSeg(const Vec2d& a1, const Vec2d& a2, const Vec2d& b1, const Vec2d& b2,
                     std::vector<Vec2d>& out)
{
    const Vec2d r = a2 - a1;
    const Vec2d s = b2 - b1;
    const double denom = r.cross(s);
    if (nearZero(denom, 1e-14))
        return;
    const double t = (b1 - a1).cross(s) / denom;
    const double u = (b1 - a1).cross(r) / denom;
    if (t < -kParamTol || t > 1 + kParamTol || u < -kParamTol || u > 1 + kParamTol)
        return;
    out.push_back(a1 + r * t);
}

void intersectLineCircle(const Vec2d& a1, const Vec2d& a2, const Vec2d& center, double r,
                         std::vector<Vec2d>& out)
{
    const Vec2d d = a2 - a1;
    const double lenSq = d.lengthSq();
    if (nearZero(lenSq))
        return;
    // Project center onto the line.
    const double t0 = (center - a1).dot(d) / lenSq;
    const Vec2d foot = a1 + d * t0;
    const double distSq = (center - foot).lengthSq();
    const double rSq = r * r;
    if (distSq > rSq + kGeomTol)
        return;
    const double half = std::sqrt(std::max(0.0, rSq - distSq)) / std::sqrt(lenSq);
    if (nearZero(half)) {
        out.push_back(foot); // tangent
        return;
    }
    out.push_back(a1 + d * (t0 - half));
    out.push_back(a1 + d * (t0 + half));
}

void intersectSegArc(const Vec2d& a1, const Vec2d& a2, const Vec2d& center, double r,
                     double a0, double sweep, std::vector<Vec2d>& out)
{
    std::vector<Vec2d> onCircle;
    intersectLineCircle(a1, a2, center, r, onCircle);
    const Vec2d d = a2 - a1;
    const double lenSq = d.lengthSq();
    for (const Vec2d& p : onCircle) {
        const double t = (p - a1).dot(d) / lenSq;
        if (t < -kParamTol || t > 1 + kParamTol)
            continue;
        if (sweep >= 2.0 * M_PI - kGeomTol ||
            angleOnArc((p - center).angle(), a0, sweep))
            out.push_back(p);
    }
}

void intersectCircleCircle(const Vec2d& c1, double r1, const Vec2d& c2, double r2,
                           std::vector<Vec2d>& out)
{
    const double d = c1.distanceTo(c2);
    if (nearZero(d) || d > r1 + r2 + kGeomTol || d < std::fabs(r1 - r2) - kGeomTol)
        return;
    const double a = (r1 * r1 - r2 * r2 + d * d) / (2 * d);
    const double hSq = r1 * r1 - a * a;
    const Vec2d dir = (c2 - c1) / d;
    const Vec2d mid = c1 + dir * a;
    if (hSq < kGeomTol) {
        out.push_back(mid); // tangent
        return;
    }
    const double h = std::sqrt(hSq);
    out.push_back(mid + dir.perp() * h);
    out.push_back(mid - dir.perp() * h);
}

void intersectArcArc(const Vec2d& c1, double r1, double s1, double w1, const Vec2d& c2,
                     double r2, double s2, double w2, std::vector<Vec2d>& out)
{
    std::vector<Vec2d> pts;
    intersectCircleCircle(c1, r1, c2, r2, pts);
    for (const Vec2d& p : pts) {
        const bool on1 = w1 >= 2.0 * M_PI - kGeomTol ||
                         angleOnArc((p - c1).angle(), s1, w1);
        const bool on2 = w2 >= 2.0 * M_PI - kGeomTol ||
                         angleOnArc((p - c2).angle(), s2, w2);
        if (on1 && on2)
            out.push_back(p);
    }
}

} // namespace viki
