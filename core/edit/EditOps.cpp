#include "EditOps.h"

#include <algorithm>

#include "doc/Entities.h"
#include "doc/EntitiesEx.h"
#include "geom/GeomUtil.h"
#include "geom/Intersect.h"

namespace viki {
namespace editops {
namespace {

// ---- decomposition ----------------------------------------------------------

struct CutSeg {
    bool isArc = false;
    Vec2d a, b;          // segment
    Vec2d c;             // arc center
    double r = 0, a0 = 0, sweep = 0;
};

void decompose(const Entity& e, std::vector<CutSeg>& out)
{
    if (const auto* line = dynamic_cast<const LineEntity*>(&e)) {
        out.push_back({false, line->p1(), line->p2(), {}, 0, 0, 0});
    } else if (const auto* xl = dynamic_cast<const XLineEntity*>(&e)) {
        out.push_back({false, xl->basePoint() - xl->direction() * 1e7,
                       xl->basePoint() + xl->direction() * 1e7, {}, 0, 0, 0});
    } else if (const auto* c = dynamic_cast<const CircleEntity*>(&e)) {
        out.push_back({true, {}, {}, c->center(), c->radius(), 0, 2.0 * M_PI});
    } else if (const auto* arc = dynamic_cast<const ArcEntity*>(&e)) {
        out.push_back({true, {}, {}, arc->center(), arc->radius(), arc->startAngle(),
                       arc->sweep()});
    } else if (const auto* pl = dynamic_cast<const PolylineEntity*>(&e)) {
        const auto& vs = pl->vertices();
        const size_t n = vs.size();
        for (size_t i = 0; i < n; ++i) {
            const bool last = (i + 1 == n);
            if (last && !pl->isClosed())
                break;
            const Vec2d a = vs[i].pos;
            const Vec2d b = vs[(i + 1) % n].pos;
            const double bulge = vs[i].bulge;
            if (nearZero(bulge)) {
                out.push_back({false, a, b, {}, 0, 0, 0});
            } else {
                // Same math as EntitiesEx's bulgeToArc, normalized to CCW.
                const double sweep = 4.0 * std::atan(bulge);
                const double chord = a.distanceTo(b);
                const double radius = chord / (2.0 * std::fabs(std::sin(sweep / 2.0)));
                const Vec2d mid = (a + b) * 0.5;
                const double h =
                    radius * std::cos(sweep / 2.0) * (bulge > 0 ? 1.0 : -1.0);
                const Vec2d center = mid + (b - a).normalized().perp() * h;
                double start = (a - center).angle();
                double w = sweep;
                if (w < 0) { // store as CCW
                    start = normalizeAngle(start + w);
                    w = -w;
                }
                out.push_back({true, {}, {}, center, radius, normalizeAngle(start), w});
            }
        }
    } else {
        // Ellipse, spline, anything else: flattened segments (fine tolerance).
        RenderContext ctx;
        ctx.chordTolerance = 1e-4;
        PrimitiveList list;
        e.buildPrimitives(ctx, list);
        for (const StrokePrimitive& s : list.strokes) {
            for (size_t i = 0; i + 1 < s.points.size(); ++i)
                out.push_back({false, s.points[i], s.points[i + 1], {}, 0, 0, 0});
            if (s.closed && s.points.size() > 1)
                out.push_back({false, s.points.back(), s.points.front(), {}, 0, 0, 0});
        }
    }
}

void intersectCutSegs(const CutSeg& x, const CutSeg& y, std::vector<Vec2d>& out)
{
    if (!x.isArc && !y.isArc)
        intersectSegSeg(x.a, x.b, y.a, y.b, out);
    else if (!x.isArc && y.isArc)
        intersectSegArc(x.a, x.b, y.c, y.r, y.a0, y.sweep, out);
    else if (x.isArc && !y.isArc)
        intersectSegArc(y.a, y.b, x.c, x.r, x.a0, x.sweep, out);
    else
        intersectArcArc(x.c, x.r, x.a0, x.sweep, y.c, y.r, y.a0, y.sweep, out);
}

std::vector<Vec2d> intersectEntityWith(const Entity& target,
                                       const std::vector<const Entity*>& others)
{
    std::vector<CutSeg> mine, theirs;
    decompose(target, mine);
    for (const Entity* o : others)
        decompose(*o, theirs);
    std::vector<Vec2d> pts;
    for (const CutSeg& m : mine)
        for (const CutSeg& t : theirs)
            intersectCutSegs(m, t, pts);
    return pts;
}

void copyStyle(const Entity& src, Entity& dst)
{
    dst.setLayerId(src.layerId());
    dst.setColor(src.color());
}

std::vector<const Entity*> resolve(const Document& doc, const std::vector<EntityId>& ids,
                                   EntityId exclude)
{
    std::vector<const Entity*> out;
    if (ids.empty()) {
        for (const EntityId id : doc.drawOrder())
            if (id != exclude)
                if (const Entity* e = doc.entity(id))
                    out.push_back(e);
    } else {
        for (const EntityId id : ids)
            if (id != exclude)
                if (const Entity* e = doc.entity(id))
                    out.push_back(e);
    }
    return out;
}

// Sorted unique parameter values strictly inside (min,max).
std::vector<double> insideSorted(std::vector<double> vals, double min, double max,
                                 double tol)
{
    std::vector<double> out;
    for (const double v : vals)
        if (v > min + tol && v < max - tol)
            out.push_back(v);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end(),
                          [tol](double a, double b) { return std::fabs(a - b) < tol; }),
              out.end());
    return out;
}

} // namespace

std::vector<Vec2d> intersectionsWith(const Document& doc, EntityId entity,
                                     const std::vector<EntityId>& others)
{
    const Entity* e = doc.entity(entity);
    if (!e)
        return {};
    return intersectEntityWith(*e, resolve(doc, others, entity));
}

// ---- TRIM ---------------------------------------------------------------------

OpResult trimEntity(Document& doc, EntityId target, std::vector<EntityId> cutters,
                    const Vec2d& pick)
{
    const Entity* e = doc.entity(target);
    if (!e)
        return {false, QStringLiteral("no such entity")};
    const auto others = resolve(doc, cutters, target);
    const auto pts = intersectEntityWith(*e, others);
    if (pts.empty())
        return {false, QStringLiteral("no intersections with cutting edges")};

    if (const auto* line = dynamic_cast<const LineEntity*>(e)) {
        const Vec2d a = line->p1(), d = line->p2() - line->p1();
        const double lenSq = d.lengthSq();
        std::vector<double> ts;
        for (const Vec2d& p : pts)
            ts.push_back((p - a).dot(d) / lenSq);
        ts = insideSorted(ts, 0.0, 1.0, 1e-9);
        if (ts.empty())
            return {false, QStringLiteral("no cut inside the line")};
        const double tp = std::clamp((pick - a).dot(d) / lenSq, 0.0, 1.0);
        double lo = 0.0, hi = 1.0;
        for (const double t : ts) {
            if (t <= tp)
                lo = std::max(lo, t);
            else
                hi = std::min(hi, t);
        }
        doc.removeEntity(target);
        if (lo > 1e-9) {
            auto piece = std::make_unique<LineEntity>(a, a + d * lo);
            copyStyle(*line, *piece);
            doc.addEntity(std::move(piece));
        }
        if (hi < 1.0 - 1e-9) {
            auto piece = std::make_unique<LineEntity>(a + d * hi, a + d);
            copyStyle(*line, *piece);
            doc.addEntity(std::move(piece));
        }
        return {true, {}};
    }

    if (const auto* circle = dynamic_cast<const CircleEntity*>(e)) {
        std::vector<double> angles;
        for (const Vec2d& p : pts)
            angles.push_back(normalizeAngle((p - circle->center()).angle()));
        std::sort(angles.begin(), angles.end());
        angles.erase(std::unique(angles.begin(), angles.end(),
                                 [](double x, double y) { return nearEqual(x, y, 1e-9); }),
                     angles.end());
        if (angles.size() < 2)
            return {false, QStringLiteral("circle needs two intersections to trim")};
        const double ap = normalizeAngle((pick - circle->center()).angle());
        // Find the adjacent pair whose CCW interval contains the pick.
        double from = angles.back(), to = angles.front();
        for (size_t i = 0; i < angles.size(); ++i) {
            const double s = angles[i];
            const double t = angles[(i + 1) % angles.size()];
            if (angleOnArc(ap, s, ccwSweep(s, t))) {
                from = s;
                to = t;
                break;
            }
        }
        auto piece = std::make_unique<ArcEntity>(circle->center(), circle->radius(), to,
                                                 ccwSweep(to, from));
        copyStyle(*circle, *piece);
        doc.removeEntity(target);
        doc.addEntity(std::move(piece));
        return {true, {}};
    }

    if (const auto* arc = dynamic_cast<const ArcEntity*>(e)) {
        std::vector<double> us;
        for (const Vec2d& p : pts)
            us.push_back(normalizeAngle((p - arc->center()).angle() - arc->startAngle()));
        us = insideSorted(us, 0.0, arc->sweep(), 1e-9);
        if (us.empty())
            return {false, QStringLiteral("no cut on the arc")};
        const double up = std::clamp(
            normalizeAngle((pick - arc->center()).angle() - arc->startAngle()), 0.0,
            arc->sweep());
        double lo = 0.0, hi = arc->sweep();
        for (const double u : us) {
            if (u <= up)
                lo = std::max(lo, u);
            else
                hi = std::min(hi, u);
        }
        const Vec2d c = arc->center();
        const double r = arc->radius(), s0 = arc->startAngle(), w = arc->sweep();
        doc.removeEntity(target);
        if (lo > 1e-9) {
            auto piece = std::make_unique<ArcEntity>(c, r, s0, lo);
            copyStyle(*arc, *piece);
            doc.addEntity(std::move(piece));
        }
        if (hi < w - 1e-9) {
            auto piece = std::make_unique<ArcEntity>(c, r, normalizeAngle(s0 + hi), w - hi);
            copyStyle(*arc, *piece);
            doc.addEntity(std::move(piece));
        }
        return {true, {}};
    }

    return {false, QStringLiteral("TRIM supports lines, circles and arcs "
                                  "(EXPLODE polylines first)")};
}

// ---- EXTEND -------------------------------------------------------------------

OpResult extendEntity(Document& doc, EntityId target, std::vector<EntityId> boundaries,
                      const Vec2d& pick)
{
    const Entity* e = doc.entity(target);
    if (!e)
        return {false, QStringLiteral("no such entity")};
    const auto others = resolve(doc, boundaries, target);
    std::vector<CutSeg> boundary;
    for (const Entity* o : others)
        decompose(*o, boundary);

    if (const auto* line = dynamic_cast<const LineEntity*>(e)) {
        const bool atP2 = pick.distanceTo(line->p2()) <= pick.distanceTo(line->p1());
        const Vec2d fixed = atP2 ? line->p1() : line->p2();
        const Vec2d moving = atP2 ? line->p2() : line->p1();
        const Vec2d dir = (moving - fixed).normalized();
        const double curLen = fixed.distanceTo(moving);

        std::vector<Vec2d> hits;
        const CutSeg ray{false, fixed, fixed + dir * 1e7, {}, 0, 0, 0};
        for (const CutSeg& b : boundary)
            intersectCutSegs(ray, b, hits);
        double best = std::numeric_limits<double>::max();
        Vec2d bestPt;
        for (const Vec2d& h : hits) {
            const double t = (h - fixed).dot(dir);
            if (t > curLen + 1e-9 && t < best) {
                best = t;
                bestPt = h;
            }
        }
        if (best == std::numeric_limits<double>::max())
            return {false, QStringLiteral("no boundary beyond that end")};
        auto* mut = static_cast<LineEntity*>(doc.beginModify(target));
        *mut = atP2 ? LineEntity(line->p1(), bestPt) : LineEntity(bestPt, line->p2());
        copyStyle(*e, *mut);
        doc.endModify(target);
        return {true, {}};
    }

    if (const auto* arc = dynamic_cast<const ArcEntity*>(e)) {
        std::vector<Vec2d> hits;
        const CutSeg full{true, {}, {}, arc->center(), arc->radius(), 0, 2.0 * M_PI};
        for (const CutSeg& b : boundary)
            intersectCutSegs(full, b, hits);
        const bool atEnd =
            pick.distanceTo(arc->endPoint()) <= pick.distanceTo(arc->startPoint());
        double best = std::numeric_limits<double>::max();
        for (const Vec2d& h : hits) {
            const double ang = (h - arc->center()).angle();
            // Angular distance beyond the chosen end, in the extend direction.
            const double u = atEnd
                                 ? normalizeAngle(ang - (arc->startAngle() + arc->sweep()))
                                 : normalizeAngle(arc->startAngle() - ang);
            if (u > 1e-9 && u < best && arc->sweep() + u < 2.0 * M_PI)
                best = u;
        }
        if (best == std::numeric_limits<double>::max())
            return {false, QStringLiteral("no boundary beyond that end")};
        auto* mut = static_cast<ArcEntity*>(doc.beginModify(target));
        if (atEnd)
            *mut = ArcEntity(arc->center(), arc->radius(), arc->startAngle(),
                             arc->sweep() + best);
        else
            *mut = ArcEntity(arc->center(), arc->radius(),
                             normalizeAngle(arc->startAngle() - best), arc->sweep() + best);
        copyStyle(*e, *mut);
        doc.endModify(target);
        return {true, {}};
    }

    return {false, QStringLiteral("EXTEND supports lines and arcs")};
}

// ---- OFFSET -------------------------------------------------------------------

OpResult offsetEntity(Document& doc, EntityId source, double distance, const Vec2d& sidePick)
{
    const Entity* e = doc.entity(source);
    if (!e)
        return {false, QStringLiteral("no such entity")};
    if (distance <= kGeomTol)
        return {false, QStringLiteral("offset distance must be positive")};

    std::unique_ptr<Entity> made;

    if (const auto* line = dynamic_cast<const LineEntity*>(e)) {
        const Vec2d dir = (line->p2() - line->p1()).normalized();
        const double side = dir.cross(sidePick - line->p1()) >= 0 ? 1.0 : -1.0;
        const Vec2d off = dir.perp() * (distance * side);
        made = std::make_unique<LineEntity>(line->p1() + off, line->p2() + off);
    } else if (const auto* xl = dynamic_cast<const XLineEntity*>(e)) {
        const double side = xl->direction().cross(sidePick - xl->basePoint()) >= 0 ? 1 : -1;
        const Vec2d off = xl->direction().perp() * (distance * side);
        made = std::make_unique<XLineEntity>(xl->basePoint() + off, xl->direction());
    } else if (const auto* c = dynamic_cast<const CircleEntity*>(e)) {
        const bool outside = sidePick.distanceTo(c->center()) > c->radius();
        const double r = c->radius() + (outside ? distance : -distance);
        if (r <= kGeomTol)
            return {false, QStringLiteral("offset collapses the circle")};
        made = std::make_unique<CircleEntity>(c->center(), r);
    } else if (const auto* arc = dynamic_cast<const ArcEntity*>(e)) {
        const bool outside = sidePick.distanceTo(arc->center()) > arc->radius();
        const double r = arc->radius() + (outside ? distance : -distance);
        if (r <= kGeomTol)
            return {false, QStringLiteral("offset collapses the arc")};
        made = std::make_unique<ArcEntity>(arc->center(), r, arc->startAngle(), arc->sweep());
    } else if (const auto* pl = dynamic_cast<const PolylineEntity*>(e)) {
        for (const PolyVertex& v : pl->vertices())
            if (!nearZero(v.bulge))
                return {false,
                        QStringLiteral("OFFSET of arc-segment polylines is not supported "
                                       "yet — EXPLODE first")};
        const auto& vs = pl->vertices();
        if (vs.size() < 2)
            return {false, QStringLiteral("degenerate polyline")};
        // Side: sign against the nearest segment.
        double bestDist = std::numeric_limits<double>::max();
        double side = 1.0;
        const size_t n = vs.size();
        const size_t segCount = pl->isClosed() ? n : n - 1;
        for (size_t i = 0; i < segCount; ++i) {
            const Vec2d a = vs[i].pos, b = vs[(i + 1) % n].pos;
            const double dist = distanceToSegment(sidePick, a, b);
            if (dist < bestDist) {
                bestDist = dist;
                side = (b - a).cross(sidePick - a) >= 0 ? 1.0 : -1.0;
            }
        }
        // Offset each segment, join consecutive ones by line intersection.
        std::vector<Vec2d> offA(segCount), offB(segCount);
        for (size_t i = 0; i < segCount; ++i) {
            const Vec2d a = vs[i].pos, b = vs[(i + 1) % n].pos;
            const Vec2d off = (b - a).normalized().perp() * (distance * side);
            offA[i] = a + off;
            offB[i] = b + off;
        }
        std::vector<PolyVertex> out;
        const auto joint = [&](size_t i, size_t j) -> Vec2d {
            std::vector<Vec2d> x;
            intersectLinesInf(offA[i], offB[i], offA[j], offB[j], x);
            return x.empty() ? offB[i] : x.front(); // parallel: fall back to butt joint
        };
        if (pl->isClosed()) {
            for (size_t i = 0; i < segCount; ++i)
                out.push_back({joint((i + segCount - 1) % segCount, i), 0.0});
        } else {
            out.push_back({offA[0], 0.0});
            for (size_t i = 0; i + 1 < segCount; ++i)
                out.push_back({joint(i, i + 1), 0.0});
            out.push_back({offB[segCount - 1], 0.0});
        }
        made = std::make_unique<PolylineEntity>(std::move(out), pl->isClosed());
    } else {
        return {false, QStringLiteral("OFFSET does not support this entity type yet")};
    }

    copyStyle(*e, *made);
    doc.addEntity(std::move(made));
    return {true, {}};
}

// ---- FILLET / CHAMFER -----------------------------------------------------------

namespace {

struct CornerPrep {
    Vec2d P;      // intersection of the infinite lines
    Vec2d e1, e2; // unit directions from P toward the kept halves
    Vec2d far1, far2; // kept far endpoints
    double angle; // between e1 and e2, (0, pi)
};

std::optional<CornerPrep> prepCorner(const LineEntity& l1, const Vec2d& pick1,
                                     const LineEntity& l2, const Vec2d& pick2)
{
    std::vector<Vec2d> x;
    intersectLinesInf(l1.p1(), l1.p2(), l2.p1(), l2.p2(), x);
    if (x.empty())
        return std::nullopt;
    CornerPrep cp;
    cp.P = x.front();
    const auto pickSide = [&](const LineEntity& l, const Vec2d& pick, Vec2d& e, Vec2d& far) {
        const Vec2d d = (l.p2() - l.p1()).normalized();
        const double tPick = (pick - cp.P).dot(d);
        e = tPick >= 0 ? d : d * -1.0;
        const double t1 = (l.p1() - cp.P).dot(e);
        const double t2 = (l.p2() - cp.P).dot(e);
        far = t1 >= t2 ? l.p1() : l.p2();
    };
    pickSide(l1, pick1, cp.e1, cp.far1);
    pickSide(l2, pick2, cp.e2, cp.far2);
    cp.angle = std::acos(std::clamp(cp.e1.dot(cp.e2), -1.0, 1.0));
    if (cp.angle < 1e-6 || cp.angle > M_PI - 1e-6)
        return std::nullopt;
    return cp;
}

} // namespace

OpResult filletLines(Document& doc, EntityId line1, const Vec2d& pick1, EntityId line2,
                     const Vec2d& pick2, double radius)
{
    auto* l1 = dynamic_cast<LineEntity*>(doc.entity(line1));
    auto* l2 = dynamic_cast<LineEntity*>(doc.entity(line2));
    if (!l1 || !l2 || line1 == line2)
        return {false, QStringLiteral("FILLET needs two distinct lines")};
    const auto cp = prepCorner(*l1, pick1, *l2, pick2);
    if (!cp)
        return {false, QStringLiteral("lines are parallel or degenerate")};

    Vec2d t1 = cp->P, t2 = cp->P;
    if (radius > kGeomTol) {
        const double tangentLen = radius / std::tan(cp->angle / 2.0);
        t1 = cp->P + cp->e1 * tangentLen;
        t2 = cp->P + cp->e2 * tangentLen;
        const Vec2d bis = (cp->e1 + cp->e2).normalized();
        const Vec2d center = cp->P + bis * (radius / std::sin(cp->angle / 2.0));
        double s = (t1 - center).angle();
        double e = (t2 - center).angle();
        double sweep = ccwSweep(s, e);
        if (sweep > M_PI) { // fillet arc is always the short way
            std::swap(s, e);
            sweep = ccwSweep(s, e);
        }
        auto arc = std::make_unique<ArcEntity>(center, radius, s, sweep);
        copyStyle(*l1, *arc);
        doc.addEntity(std::move(arc));
    }

    auto* m1 = static_cast<LineEntity*>(doc.beginModify(line1));
    *m1 = LineEntity(cp->far1, t1);
    copyStyle(*l1, *m1);
    doc.endModify(line1);
    auto* m2 = static_cast<LineEntity*>(doc.beginModify(line2));
    *m2 = LineEntity(cp->far2, t2);
    copyStyle(*l2, *m2);
    doc.endModify(line2);
    return {true, {}};
}

OpResult chamferLines(Document& doc, EntityId line1, const Vec2d& pick1, EntityId line2,
                      const Vec2d& pick2, double d1, double d2)
{
    auto* l1 = dynamic_cast<LineEntity*>(doc.entity(line1));
    auto* l2 = dynamic_cast<LineEntity*>(doc.entity(line2));
    if (!l1 || !l2 || line1 == line2)
        return {false, QStringLiteral("CHAMFER needs two distinct lines")};
    const auto cp = prepCorner(*l1, pick1, *l2, pick2);
    if (!cp)
        return {false, QStringLiteral("lines are parallel or degenerate")};

    const Vec2d t1 = cp->P + cp->e1 * d1;
    const Vec2d t2 = cp->P + cp->e2 * d2;
    if (d1 > kGeomTol || d2 > kGeomTol) {
        auto ch = std::make_unique<LineEntity>(t1, t2);
        copyStyle(*l1, *ch);
        doc.addEntity(std::move(ch));
    }
    auto* m1 = static_cast<LineEntity*>(doc.beginModify(line1));
    *m1 = LineEntity(cp->far1, t1);
    copyStyle(*l1, *m1);
    doc.endModify(line1);
    auto* m2 = static_cast<LineEntity*>(doc.beginModify(line2));
    *m2 = LineEntity(cp->far2, t2);
    copyStyle(*l2, *m2);
    doc.endModify(line2);
    return {true, {}};
}

// ---- BREAK --------------------------------------------------------------------

OpResult breakEntity(Document& doc, EntityId target, const Vec2d& p1, const Vec2d& p2)
{
    const Entity* e = doc.entity(target);
    if (!e)
        return {false, QStringLiteral("no such entity")};

    if (const auto* line = dynamic_cast<const LineEntity*>(e)) {
        const Vec2d a = line->p1(), d = line->p2() - line->p1();
        const double lenSq = d.lengthSq();
        double t1 = std::clamp((p1 - a).dot(d) / lenSq, 0.0, 1.0);
        double t2 = std::clamp((p2 - a).dot(d) / lenSq, 0.0, 1.0);
        if (t1 > t2)
            std::swap(t1, t2);
        doc.removeEntity(target);
        if (t1 > 1e-9) {
            auto piece = std::make_unique<LineEntity>(a, a + d * t1);
            copyStyle(*line, *piece);
            doc.addEntity(std::move(piece));
        }
        if (t2 < 1.0 - 1e-9) {
            auto piece = std::make_unique<LineEntity>(a + d * t2, a + d);
            copyStyle(*line, *piece);
            doc.addEntity(std::move(piece));
        }
        return {true, {}};
    }

    if (const auto* circle = dynamic_cast<const CircleEntity*>(e)) {
        const double a1 = normalizeAngle((p1 - circle->center()).angle());
        const double a2 = normalizeAngle((p2 - circle->center()).angle());
        if (nearEqual(a1, a2, 1e-9))
            return {false, QStringLiteral("break points coincide on the circle")};
        // Remove the CCW span a1 -> a2; keep the arc a2 -> a1.
        auto piece = std::make_unique<ArcEntity>(circle->center(), circle->radius(), a2,
                                                 ccwSweep(a2, a1));
        copyStyle(*circle, *piece);
        doc.removeEntity(target);
        doc.addEntity(std::move(piece));
        return {true, {}};
    }

    if (const auto* arc = dynamic_cast<const ArcEntity*>(e)) {
        double u1 = std::clamp(normalizeAngle((p1 - arc->center()).angle() - arc->startAngle()),
                               0.0, arc->sweep());
        double u2 = std::clamp(normalizeAngle((p2 - arc->center()).angle() - arc->startAngle()),
                               0.0, arc->sweep());
        if (u1 > u2)
            std::swap(u1, u2);
        const Vec2d c = arc->center();
        const double r = arc->radius(), s0 = arc->startAngle(), w = arc->sweep();
        doc.removeEntity(target);
        if (u1 > 1e-9) {
            auto piece = std::make_unique<ArcEntity>(c, r, s0, u1);
            copyStyle(*arc, *piece);
            doc.addEntity(std::move(piece));
        }
        if (u2 < w - 1e-9) {
            auto piece = std::make_unique<ArcEntity>(c, r, normalizeAngle(s0 + u2), w - u2);
            copyStyle(*arc, *piece);
            doc.addEntity(std::move(piece));
        }
        return {true, {}};
    }

    return {false, QStringLiteral("BREAK supports lines, circles and arcs")};
}

// ---- JOIN ---------------------------------------------------------------------

OpResult joinEntities(Document& doc, const std::vector<EntityId>& ids)
{
    // Collect joinable pieces (lines and arcs).
    struct Piece {
        EntityId id;
        Vec2d a, b;      // endpoints
        double bulge;    // 0 = line; arc bulge from a to b
    };
    std::vector<Piece> pieces;
    for (const EntityId id : ids) {
        const Entity* e = doc.entity(id);
        if (const auto* l = dynamic_cast<const LineEntity*>(e)) {
            pieces.push_back({id, l->p1(), l->p2(), 0.0});
        } else if (const auto* arc = dynamic_cast<const ArcEntity*>(e)) {
            pieces.push_back({id, arc->startPoint(), arc->endPoint(),
                              std::tan(arc->sweep() / 4.0)});
        }
    }
    if (pieces.size() < 2)
        return {false, QStringLiteral("JOIN needs at least two lines/arcs")};

    // Special case: all collinear lines -> one line.
    bool allLines = true;
    for (const Piece& p : pieces)
        allLines = allLines && nearZero(p.bulge);
    if (allLines) {
        const Vec2d dir = (pieces[0].b - pieces[0].a).normalized();
        bool collinear = true;
        for (const Piece& p : pieces) {
            collinear = collinear && nearZero(dir.cross(p.a - pieces[0].a), 1e-6) &&
                        nearZero(dir.cross(p.b - pieces[0].a), 1e-6);
        }
        if (collinear) {
            double tmin = std::numeric_limits<double>::max();
            double tmax = std::numeric_limits<double>::lowest();
            for (const Piece& p : pieces) {
                for (const Vec2d& q : {p.a, p.b}) {
                    const double t = (q - pieces[0].a).dot(dir);
                    tmin = std::min(tmin, t);
                    tmax = std::max(tmax, t);
                }
            }
            auto merged = std::make_unique<LineEntity>(pieces[0].a + dir * tmin,
                                                       pieces[0].a + dir * tmax);
            copyStyle(*doc.entity(pieces[0].id), *merged);
            for (const Piece& p : pieces)
                doc.removeEntity(p.id);
            doc.addEntity(std::move(merged));
            return {true, QStringLiteral("merged into one line")};
        }
    }

    // General chaining into a polyline.
    constexpr double tol = 1e-6;
    std::vector<bool> used(pieces.size(), false);
    std::vector<Piece> chain{pieces[0]};
    used[0] = true;
    bool grew = true;
    while (grew) {
        grew = false;
        for (size_t i = 0; i < pieces.size(); ++i) {
            if (used[i])
                continue;
            Piece p = pieces[i];
            if (nearEqual(chain.back().b, p.a, tol)) {
                chain.push_back(p);
            } else if (nearEqual(chain.back().b, p.b, tol)) {
                std::swap(p.a, p.b);
                p.bulge = -p.bulge;
                chain.push_back(p);
            } else if (nearEqual(chain.front().a, p.b, tol)) {
                chain.insert(chain.begin(), p);
            } else if (nearEqual(chain.front().a, p.a, tol)) {
                std::swap(p.a, p.b);
                p.bulge = -p.bulge;
                chain.insert(chain.begin(), p);
            } else {
                continue;
            }
            used[i] = true;
            grew = true;
        }
    }
    size_t usedCount = 0;
    for (const bool u : used)
        usedCount += u ? 1 : 0;
    if (usedCount < 2)
        return {false, QStringLiteral("selected entities do not connect")};

    const bool closed = nearEqual(chain.front().a, chain.back().b, tol);
    std::vector<PolyVertex> verts;
    for (const Piece& p : chain)
        verts.push_back({p.a, p.bulge});
    if (!closed)
        verts.push_back({chain.back().b, 0.0});

    auto pl = std::make_unique<PolylineEntity>(std::move(verts), closed);
    copyStyle(*doc.entity(chain.front().id), *pl);
    for (size_t i = 0; i < pieces.size(); ++i)
        if (used[i])
            doc.removeEntity(pieces[i].id);
    doc.addEntity(std::move(pl));
    return {true, QStringLiteral("joined %1 entities into a polyline").arg(usedCount)};
}

// ---- EXPLODE ------------------------------------------------------------------

OpResult explodeEntities(Document& doc, const std::vector<EntityId>& ids)
{
    int made = 0, failed = 0;
    for (const EntityId id : ids) {
        const auto* pl = dynamic_cast<const PolylineEntity*>(doc.entity(id));
        if (!pl) {
            ++failed;
            continue;
        }
        std::vector<CutSeg> segs;
        decompose(*pl, segs);
        for (const CutSeg& s : segs) {
            std::unique_ptr<Entity> piece;
            if (s.isArc)
                piece = std::make_unique<ArcEntity>(s.c, s.r, s.a0, s.sweep);
            else
                piece = std::make_unique<LineEntity>(s.a, s.b);
            copyStyle(*pl, *piece);
            doc.addEntity(std::move(piece));
            ++made;
        }
        doc.removeEntity(id);
    }
    if (made == 0)
        return {false, QStringLiteral("nothing exploded (only polylines explode in v1)")};
    QString msg = QStringLiteral("exploded into %1 entities").arg(made);
    if (failed > 0)
        msg += QStringLiteral(" (%1 not explodable)").arg(failed);
    return {true, msg};
}

// ---- STRETCH ------------------------------------------------------------------

OpResult stretchEntities(Document& doc, const std::vector<EntityId>& ids,
                         const BBox2d& window, const Vec2d& delta)
{
    int n = 0;
    for (const EntityId id : ids) {
        Entity* e = doc.beginModify(id);
        if (!e)
            continue;
        e->stretch(window, delta);
        doc.endModify(id);
        ++n;
    }
    return {true, QStringLiteral("%1 object(s) stretched").arg(n)};
}

} // namespace editops
} // namespace viki
