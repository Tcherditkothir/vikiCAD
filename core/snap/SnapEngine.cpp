#include "SnapEngine.h"

#include "geom/GeomUtil.h"

namespace viki {
namespace {

int priorityOf(SnapKind k)
{
    switch (k) {
    case SnapKind::Endpoint: return 0;
    case SnapKind::Intersection: return 1;
    case SnapKind::Midpoint: return 2;
    case SnapKind::Center: return 3;
    case SnapKind::Quadrant: return 4;
    case SnapKind::Perpendicular: return 5;
    case SnapKind::Grid: return 6;
    }
    return 7;
}

bool kindEnabled(SnapKind k, const SnapSettings& s)
{
    switch (k) {
    case SnapKind::Endpoint: return s.endpoint;
    case SnapKind::Midpoint: return s.midpoint;
    case SnapKind::Center: return s.center;
    case SnapKind::Quadrant: return s.quadrant;
    case SnapKind::Intersection: return s.intersection;
    case SnapKind::Perpendicular: return s.perpendicular;
    case SnapKind::Grid: return true;
    }
    return false;
}

PrimitiveList flatten(const Document& doc, const Entity& e, double tol,
                      const BBox2d& viewBox)
{
    RenderContext ctx;
    ctx.chordTolerance = tol;
    ctx.viewBox = viewBox;
    ctx.doc = &doc;
    PrimitiveList list;
    e.buildPrimitives(ctx, list);
    return list;
}

// Collect the segments of a primitive list.
void collectSegments(const PrimitiveList& list,
                     std::vector<std::pair<Vec2d, Vec2d>>& out)
{
    for (const StrokePrimitive& s : list.strokes) {
        const size_t n = s.points.size();
        for (size_t i = 0; i + 1 < n; ++i)
            out.push_back({s.points[i], s.points[i + 1]});
        if (s.closed && n > 1)
            out.push_back({s.points[n - 1], s.points[0]});
    }
}

std::optional<Vec2d> segmentIntersection(const Vec2d& a1, const Vec2d& a2,
                                         const Vec2d& b1, const Vec2d& b2)
{
    const Vec2d r = a2 - a1;
    const Vec2d s = b2 - b1;
    const double denom = r.cross(s);
    if (nearZero(denom, 1e-14))
        return std::nullopt;
    const double t = (b1 - a1).cross(s) / denom;
    const double u = (b1 - a1).cross(r) / denom;
    if (t < -1e-9 || t > 1 + 1e-9 || u < -1e-9 || u > 1 + 1e-9)
        return std::nullopt;
    return a1 + r * t;
}

Vec2d closestOnSegments(const std::vector<std::pair<Vec2d, Vec2d>>& segs, const Vec2d& p)
{
    Vec2d best = p;
    double bestDist = std::numeric_limits<double>::max();
    for (const auto& [a, b] : segs) {
        const Vec2d ab = b - a;
        const double lenSq = ab.lengthSq();
        const double t = nearZero(lenSq) ? 0.0
                                         : std::clamp((p - a).dot(ab) / lenSq, 0.0, 1.0);
        const Vec2d q = a + ab * t;
        const double d = p.distanceTo(q);
        if (d < bestDist) {
            bestDist = d;
            best = q;
        }
    }
    return best;
}

} // namespace

std::optional<SnapResult> snapQuery(const Document& doc, const Vec2d& cursor,
                                    double tolerance, const SnapSettings& settings,
                                    const std::optional<Vec2d>& perpBase)
{
    if (!settings.enabled)
        return std::nullopt;

    const BBox2d probe = BBox2d{cursor, cursor}.inflated(tolerance);

    // Candidate entities: bbox prefilter (linear scan — µs at 10k entities;
    // swap in a spatial index behind this same loop if M6 profiling asks).
    struct Cand {
        const Entity* e;
        std::vector<std::pair<Vec2d, Vec2d>> segs;
    };
    std::vector<Cand> cands;
    for (const EntityId id : doc.drawOrder()) {
        const Entity* e = doc.entity(id);
        if (!e)
            continue;
        const Layer* layer = doc.layer(e->layerId());
        if (layer && !layer->visible)
            continue;
        if (!e->bounds().inflated(tolerance).contains(cursor))
            continue;
        cands.push_back({e, {}});
    }

    std::optional<SnapResult> best;
    const auto consider = [&](const Vec2d& p, SnapKind kind, EntityId id) {
        if (!kindEnabled(kind, settings) || cursor.distanceTo(p) > tolerance)
            return;
        if (!best ||
            std::make_pair(priorityOf(kind), cursor.distanceTo(p)) <
                std::make_pair(priorityOf(best->kind), cursor.distanceTo(best->point)))
            best = SnapResult{p, kind, id};
    };

    // Typed candidates from the entities themselves.
    std::vector<SnapPoint> pts;
    for (const Cand& c : cands) {
        pts.clear();
        c.e->snapPoints(pts);
        for (const SnapPoint& sp : pts)
            consider(sp.p, sp.kind, c.e->id());
    }

    // Intersections: pairwise over flattened candidates near the cursor.
    if (settings.intersection && cands.size() >= 2) {
        const BBox2d flattenBox = probe.inflated(tolerance * 4);
        for (Cand& c : cands)
            collectSegments(flatten(doc, *c.e, tolerance * 0.05, flattenBox), c.segs);
        for (size_t i = 0; i < cands.size(); ++i) {
            for (size_t j = i + 1; j < cands.size(); ++j) {
                for (const auto& [a1, a2] : cands[i].segs) {
                    // Skip segments far from the cursor.
                    if (!BBox2d(a1, a2).inflated(tolerance).contains(cursor))
                        continue;
                    for (const auto& [b1, b2] : cands[j].segs) {
                        if (const auto p = segmentIntersection(a1, a2, b1, b2))
                            consider(*p, SnapKind::Intersection, cands[i].e->id());
                    }
                }
            }
        }
    }

    // Perpendicular foot from the rubber-band base.
    if (settings.perpendicular && perpBase) {
        for (Cand& c : cands) {
            if (c.segs.empty())
                collectSegments(flatten(doc, *c.e, tolerance * 0.05,
                                        probe.inflated(tolerance * 4)),
                                c.segs);
            const Vec2d foot = closestOnSegments(c.segs, *perpBase);
            consider(foot, SnapKind::Perpendicular, c.e->id());
        }
    }

    return best;
}

} // namespace viki
