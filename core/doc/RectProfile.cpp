#include "RectProfile.h"

#include <cmath>

namespace viki {

std::optional<RectProfile> rectProfileOf(const PolylineEntity& pl)
{
    // |cos| tolerance on the corner angles: 1e-6 rad off square still reads
    // as a rectangle (float noise from imports/rotations), anything more is
    // an honest parallelogram and gets NO rectangle rows.
    constexpr double kSquareTol = 1e-6;
    const auto& vs = pl.vertices();
    if (!pl.isClosed() || vs.size() != 4)
        return std::nullopt;
    for (const PolyVertex& v : vs)
        if (!nearZero(v.bulge))
            return std::nullopt; // an arc side is never a rectangle side

    Vec2d edge[4];
    for (int i = 0; i < 4; ++i) {
        edge[i] = vs[(i + 1) % 4].pos - vs[i].pos;
        if (edge[i].length() <= kGeomTol)
            return std::nullopt; // degenerate side
    }
    for (int i = 0; i < 4; ++i) {
        const Vec2d a = edge[i].normalized();
        const Vec2d b = edge[(i + 1) % 4].normalized();
        if (std::fabs(a.dot(b)) > kSquareTol)
            return std::nullopt; // corner not square
    }

    RectProfile r;
    // Anchor: the vertex closest to the world origin, ties -> lowest index
    // (see RectProfile.h for why).
    double best = vs[0].pos.lengthSq();
    for (int i = 1; i < 4; ++i) {
        const double d = vs[i].pos.lengthSq();
        if (d < best - 1e-12) {
            best = d;
            r.anchorIndex = i;
        }
    }
    const int i = r.anchorIndex;
    r.anchor = vs[i].pos;
    const Vec2d fwd = vs[(i + 1) % 4].pos - r.anchor;
    const Vec2d back = vs[(i + 3) % 4].pos - r.anchor;
    r.sideFwd = fwd.length();
    r.sideBack = back.length();
    r.dirFwd = fwd.normalized();
    r.dirBack = back.normalized();
    r.longIsFwd = r.sideFwd >= r.sideBack - 1e-12;
    return r;
}

bool applyRectDims(PolylineEntity& pl, double newLength, double newHeight)
{
    const auto r = rectProfileOf(pl);
    if (!r || newLength <= kGeomTol || newHeight <= kGeomTol)
        return false;
    const double sFwd = r->longIsFwd ? newLength : newHeight;
    const double sBack = r->longIsFwd ? newHeight : newLength;
    auto vs = pl.vertices();
    const int i = r->anchorIndex;
    vs[(i + 1) % 4].pos = r->anchor + r->dirFwd * sFwd;
    vs[(i + 2) % 4].pos = r->anchor + r->dirFwd * sFwd + r->dirBack * sBack;
    vs[(i + 3) % 4].pos = r->anchor + r->dirBack * sBack;
    pl.setVertices(std::move(vs));
    return true;
}

} // namespace viki
