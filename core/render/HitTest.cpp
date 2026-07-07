#include "HitTest.h"

#include "geom/GeomUtil.h"

namespace viki {
namespace hittest {
namespace {

// Flatten with a tolerance tied to the pick tolerance so curve error stays
// far below what a user can perceive.
PrimitiveList flatten(const Entity& e, double chordTol)
{
    RenderContext ctx;
    ctx.chordTolerance = chordTol;
    PrimitiveList list;
    e.buildPrimitives(ctx, list);
    return list;
}

double distanceToPrimitives(const PrimitiveList& list, const Vec2d& p)
{
    double best = std::numeric_limits<double>::max();
    for (const StrokePrimitive& s : list.strokes) {
        const size_t n = s.points.size();
        for (size_t i = 0; i + 1 < n; ++i)
            best = std::min(best, distanceToSegment(p, s.points[i], s.points[i + 1]));
        if (s.closed && n > 1)
            best = std::min(best, distanceToSegment(p, s.points[n - 1], s.points[0]));
    }
    return best;
}

bool segmentIntersectsBox(const Vec2d& a, const Vec2d& b, const BBox2d& box)
{
    if (box.contains(a) || box.contains(b))
        return true;
    if (!BBox2d(a, b).intersects(box))
        return false;
    // Test against the four box edges via signed areas.
    const Vec2d c1 = box.min;
    const Vec2d c2{box.max.x, box.min.y};
    const Vec2d c3 = box.max;
    const Vec2d c4{box.min.x, box.max.y};
    const auto crosses = [&](const Vec2d& p, const Vec2d& q) {
        const double d1 = (b - a).cross(p - a);
        const double d2 = (b - a).cross(q - a);
        const double d3 = (q - p).cross(a - p);
        const double d4 = (q - p).cross(b - p);
        return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
    };
    return crosses(c1, c2) || crosses(c2, c3) || crosses(c3, c4) || crosses(c4, c1);
}

bool primitivesTouchBox(const PrimitiveList& list, const BBox2d& box)
{
    for (const StrokePrimitive& s : list.strokes) {
        const size_t n = s.points.size();
        for (size_t i = 0; i + 1 < n; ++i)
            if (segmentIntersectsBox(s.points[i], s.points[i + 1], box))
                return true;
        if (s.closed && n > 1 && segmentIntersectsBox(s.points[n - 1], s.points[0], box))
            return true;
    }
    return false;
}

} // namespace

EntityId pick(const Document& doc, const Vec2d& point, double tolerance)
{
    EntityId best = kInvalidEntityId;
    double bestDist = tolerance;
    // Later in draw order wins ties (matches what the user sees on top).
    for (const EntityId id : doc.drawOrder()) {
        const Entity* e = doc.entity(id);
        if (!e || !e->bounds().inflated(tolerance).contains(point))
            continue;
        const double d = distanceToPrimitives(flatten(*e, tolerance * 0.1), point);
        if (d <= bestDist) {
            bestDist = d;
            best = id;
        }
    }
    return best;
}

std::vector<EntityId> window(const Document& doc, const BBox2d& box)
{
    std::vector<EntityId> out;
    for (const EntityId id : doc.drawOrder()) {
        const Entity* e = doc.entity(id);
        // Geometry ⊆ bounds, so bounds inside the box means fully inside.
        if (e && box.contains(e->bounds()))
            out.push_back(id);
    }
    return out;
}

std::vector<EntityId> crossing(const Document& doc, const BBox2d& box)
{
    std::vector<EntityId> out;
    const double tol = std::max(box.width(), box.height()) * 1e-4;
    for (const EntityId id : doc.drawOrder()) {
        const Entity* e = doc.entity(id);
        if (!e)
            continue;
        if (box.contains(e->bounds()) ||
            primitivesTouchBox(flatten(*e, tol), box))
            out.push_back(id);
    }
    return out;
}

} // namespace hittest
} // namespace viki
