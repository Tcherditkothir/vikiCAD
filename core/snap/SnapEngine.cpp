#include "SnapEngine.h"

#include "doc/Block.h"
#include "doc/Entities.h"
#include "geom/GeomUtil.h"

namespace viki {
namespace {

int priorityOf(SnapKind k)
{
    switch (k) {
    case SnapKind::Endpoint: return 0;
    case SnapKind::Node: return 1;
    case SnapKind::Intersection: return 2;
    case SnapKind::Midpoint: return 3;
    case SnapKind::Center: return 4;
    case SnapKind::Quadrant: return 5;
    case SnapKind::Perpendicular: return 6;
    case SnapKind::Tangent: return 7;
    case SnapKind::Nearest: return 8;
    case SnapKind::Grid: return 9;
    }
    return 10;
}

bool kindEnabled(SnapKind k, const SnapSettings& s)
{
    switch (k) {
    case SnapKind::Endpoint: return s.endpoint;
    case SnapKind::Node: return s.node;
    case SnapKind::Midpoint: return s.midpoint;
    case SnapKind::Center: return s.center;
    case SnapKind::Quadrant: return s.quadrant;
    case SnapKind::Intersection: return s.intersection;
    case SnapKind::Perpendicular: return s.perpendicular;
    case SnapKind::Tangent: return s.tangent;
    case SnapKind::Nearest: return s.nearest;
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

// Typed snap points of an entity. Inserts recurse into their block definition
// so endpoints/midpoints/centers INSIDE blocks are snappable; sub-entity points
// are mapped through the insert transform (points transform exactly — no need
// to clone whole entities per query).
void collectSnapPoints(const Document& doc, const Entity& e,
                       std::vector<SnapPoint>& out, int depth = 0)
{
    const auto* ins = dynamic_cast<const InsertEntity*>(&e);
    if (!ins) {
        e.snapPoints(out);
        return;
    }
    out.push_back({ins->position, SnapKind::Endpoint});
    const BlockDef* def = depth < 4 ? doc.blockByName(ins->blockName) : nullptr;
    if (!def)
        return;
    const Xform2d xf = ins->insertXform(def->basePoint);
    std::vector<SnapPoint> sub;
    for (const auto& child : def->entities)
        collectSnapPoints(doc, *child, sub, depth + 1);
    out.reserve(out.size() + sub.size());
    for (const SnapPoint& sp : sub)
        out.push_back({xf.apply(sp.p), sp.kind});
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
        if (!doc.entityBounds(*e).inflated(tolerance).contains(cursor))
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

    // Typed candidates from the entities themselves (inserts recurse into
    // their block definitions — see collectSnapPoints).
    std::vector<SnapPoint> pts;
    for (const Cand& c : cands) {
        pts.clear();
        collectSnapPoints(doc, *c.e, pts);
        for (const SnapPoint& sp : pts)
            consider(sp.p, sp.kind, c.e->id());
    }

    // Extra reference targets (e.g. the sketch-on-face outline): vertices and
    // arc/circle centers fed in by the front-end so the profile can snap to
    // real face features. They belong to no entity (kInvalidEntityId).
    for (const SnapPoint& sp : doc.extraSnapPoints())
        consider(sp.p, sp.kind, kInvalidEntityId);

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

    // Tangent from the rubber-band base to a circle/arc: the two points on the
    // circle where the line base->point is perpendicular to the radius. Only the
    // one nearest the cursor is offered as a snap candidate. On an arc, the
    // tangent point must lie within the arc's angular sweep.
    if (settings.tangent && perpBase) {
        for (const Cand& c : cands) {
            Vec2d center;
            double radius = 0.0;
            const ArcEntity* arc = nullptr;
            if (const auto* ci = dynamic_cast<const CircleEntity*>(c.e)) {
                center = ci->center();
                radius = ci->radius();
            } else if ((arc = dynamic_cast<const ArcEntity*>(c.e))) {
                center = arc->center();
                radius = arc->radius();
            } else {
                continue;
            }
            const Vec2d d = center - *perpBase;
            const double dist = d.length();
            if (dist <= radius + 1e-12) // base inside/on circle: no real tangent
                continue;
            // Tangent length and the half-angle between center-line and tangent.
            const double baseAngle = d.angle();
            const double alpha = std::acos(radius / dist);
            for (const double sign : {+1.0, -1.0}) {
                const double a = baseAngle + M_PI + sign * alpha;
                const Vec2d tp = center + Vec2d::polar(radius, a);
                if (arc) {
                    const double rel =
                        normalizeAngle((tp - center).angle() - arc->startAngle());
                    if (rel > std::abs(arc->sweep()) + 1e-9)
                        continue;
                }
                consider(tp, SnapKind::Tangent, c.e->id());
            }
        }
    }

    // Nearest point on any entity to the cursor (lowest priority — a fallback).
    if (settings.nearest) {
        for (Cand& c : cands) {
            if (c.segs.empty())
                collectSegments(flatten(doc, *c.e, tolerance * 0.05,
                                        probe.inflated(tolerance * 4)),
                                c.segs);
            const Vec2d q = closestOnSegments(c.segs, cursor);
            consider(q, SnapKind::Nearest, c.e->id());
        }
    }

    return best;
}

} // namespace viki
