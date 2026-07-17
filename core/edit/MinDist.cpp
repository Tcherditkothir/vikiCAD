#include "MinDist.h"

#include <limits>

#include "doc/Block.h"
#include "doc/Entities.h"
#include "doc/EntitiesEx.h"
#include "geom/GeomUtil.h"

namespace viki {
namespace measure {
namespace {

// Chord sagitta for curve flattening while measuring. Gerber coordinates
// themselves carry ~1e-6 mm resolution; 1e-4 keeps the flattening error two
// orders below the 3-decimals output.
constexpr double kMeasureTol = 1e-4;

// ---- material model ---------------------------------------------------------
// An entity's material is a soup of three primitive families:
//   capsule = segment swept by a disk (wide trace piece; r=0 -> bare wire)
//   disk    = filled circle (drill hit, round pad)
//   polyset = filled polygon rings, UNION (aperture footprint, pour). This
//             matches the SOLID hatch renderer, which fills each ring
//             individually: overlapping rings (Altium RoundedRect macros =
//             2 rects + 4 corner disks) are material everywhere. Even-odd
//             parity here would flip a doubly-covered point to "outside".

struct Capsule {
    Vec2d a, b;
    double r;
};

struct Disk {
    Vec2d c;
    double r;
};

struct PolySet {
    std::vector<std::vector<Vec2d>> rings;
};

struct MShape {
    std::vector<Capsule> capsules;
    std::vector<Disk> disks;
    std::vector<PolySet> polys;
    bool approx = false; // true = built from a bounding box

    bool empty() const { return capsules.empty() && disks.empty() && polys.empty(); }
};

void addBBoxPoly(MShape& out, const BBox2d& box)
{
    if (!box.isValid())
        return;
    PolySet ps;
    ps.rings.push_back({{box.min.x, box.min.y},
                        {box.max.x, box.min.y},
                        {box.max.x, box.max.y},
                        {box.min.x, box.max.y}});
    out.polys.push_back(std::move(ps));
    out.approx = true;
}

// Flatten any entity through its own renderer: filled closed strokes become
// polygon rings (one even-odd set per entity), open/outline strokes become
// capsule chains carrying the stroke's world width.
void addFromPrimitives(const Document& doc, const Entity& e, MShape& out)
{
    RenderContext ctx;
    ctx.chordTolerance = kMeasureTol;
    ctx.doc = &doc;
    PrimitiveList list;
    e.buildPrimitives(ctx, list);

    bool contributed = false;
    PolySet filled;
    for (const StrokePrimitive& s : list.strokes) {
        if (s.points.size() < 2)
            continue;
        contributed = true;
        if (s.filled && s.closed) {
            filled.rings.push_back(s.points);
            continue;
        }
        const double r = s.width * 0.5;
        for (size_t i = 0; i + 1 < s.points.size(); ++i)
            out.capsules.push_back({s.points[i], s.points[i + 1], r});
        if (s.closed)
            out.capsules.push_back({s.points.back(), s.points.front(), r});
    }
    if (!filled.rings.empty())
        out.polys.push_back(std::move(filled));

    if (!contributed)
        addBBoxPoly(out, doc.entityBounds(e)); // text-only entities etc.
}

void buildShape(const Document& doc, const Entity& e, MShape& out, int depth)
{
    if (const auto* c = dynamic_cast<const CircleEntity*>(&e)) {
        out.disks.push_back({c->center(), c->radius()});
        return;
    }
    if (const auto* p = dynamic_cast<const PointEntity*>(&e)) {
        out.disks.push_back({p->position(), 0.0});
        return;
    }
    if (const auto* ins = dynamic_cast<const InsertEntity*>(&e)) {
        const BlockDef* def = depth < 4 ? doc.blockByName(ins->blockName) : nullptr;
        if (!def || def->entities.empty()) {
            addBBoxPoly(out, doc.entityBounds(e));
            return;
        }
        const Xform2d xf = ins->insertXform(def->basePoint);
        for (const auto& child : def->entities) {
            if (dynamic_cast<const AttDefEntity*>(child.get()))
                continue; // display-only inside pads/blocks
            auto ghost = child->clone();
            ghost->transform(xf);
            buildShape(doc, *ghost, out, depth + 1);
        }
        if (out.empty())
            addBBoxPoly(out, doc.entityBounds(e));
        return;
    }
    addFromPrimitives(doc, e, out);
}

// ---- distance kernel --------------------------------------------------------

struct Best {
    double d = std::numeric_limits<double>::max(); // edge-to-edge, may go < 0
    Vec2d pa, pb;                                  // material edge points
};

// Offset the raw closest pair (p on A's spine, q on B's spine) onto the
// material edges: each side advances by its own radius along p->q.
void consider(Best& best, double raw, const Vec2d& p, const Vec2d& q, double ra,
              double rb)
{
    const double d = raw - ra - rb;
    if (d >= best.d)
        return;
    best.d = d;
    if (raw > 1e-12) {
        const Vec2d dir = (q - p) / raw;
        best.pa = p + dir * ra;
        best.pb = q - dir * rb;
    } else {
        best.pa = best.pb = p; // spines touch: one contact point
    }
}

void capsuleVsCapsule(Best& best, const Capsule& a, const Capsule& b)
{
    Vec2d p, q;
    const double raw = closestSegSeg(a.a, a.b, b.a, b.b, p, q);
    consider(best, raw, p, q, a.r, b.r);
}

void diskVsCapsule(Best& best, const Disk& d, const Capsule& c, bool diskIsA)
{
    const Vec2d q = closestPointOnSegment(d.c, c.a, c.b);
    const double raw = d.c.distanceTo(q);
    if (diskIsA)
        consider(best, raw, d.c, q, d.r, c.r);
    else
        consider(best, raw, q, d.c, c.r, d.r);
}

void diskVsDisk(Best& best, const Disk& a, const Disk& b)
{
    consider(best, a.c.distanceTo(b.c), a.c, b.c, a.r, b.r);
}

// Polygon boundaries as zero-radius capsules (rings are closed).
void appendRingCapsules(const PolySet& ps, std::vector<Capsule>& out)
{
    for (const auto& ring : ps.rings) {
        const size_t n = ring.size();
        if (n < 2)
            continue;
        for (size_t i = 0; i < n; ++i)
            out.push_back({ring[i], ring[(i + 1) % n], 0.0});
    }
}

// Union semantics: the point is material as soon as ANY ring contains it
// (even-odd only WITHIN a ring, which is a plain polygon). Cross-ring parity
// would misclassify points covered by 2 overlapping rings of a macro pad.
bool insideUnion(const Vec2d& p, const PolySet& ps)
{
    for (const auto& ring : ps.rings) {
        bool inside = false;
        const size_t n = ring.size();
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            const Vec2d& a = ring[i];
            const Vec2d& b = ring[j];
            if ((a.y > p.y) != (b.y > p.y) &&
                p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
                inside = !inside;
        }
        if (inside)
            return true;
    }
    return false;
}

// One representative point per primitive of `s` (per RING for polysets:
// rings are unioned, any single ring may be the buried one) — enough for
// the "entirely inside a polygon" containment test (partial overlaps are
// already caught by negative boundary distances).
void representativePoints(const MShape& s, std::vector<Vec2d>& out)
{
    for (const Disk& d : s.disks)
        out.push_back(d.c);
    for (const Capsule& c : s.capsules)
        out.push_back(c.a);
    for (const PolySet& ps : s.polys)
        for (const auto& ring : ps.rings)
            if (!ring.empty())
                out.push_back(ring.front());
}

bool containedInPolys(const MShape& host, const MShape& guest, Vec2d& where)
{
    if (host.polys.empty())
        return false;
    std::vector<Vec2d> reps;
    representativePoints(guest, reps);
    for (const PolySet& ps : host.polys)
        for (const Vec2d& p : reps)
            if (insideUnion(p, ps)) {
                where = p;
                return true;
            }
    return false;
}

} // namespace

MinDistResult minDistance(const Document& doc, EntityId aId, EntityId bId)
{
    MinDistResult res;
    const Entity* ea = doc.entity(aId);
    const Entity* eb = doc.entity(bId);
    if (!ea || !eb) {
        res.error = QStringLiteral("no entity #%1").arg(ea ? bId : aId);
        return res;
    }
    if (aId == bId) {
        res.error = QStringLiteral("pick two DIFFERENT entities");
        return res;
    }

    MShape sa, sb;
    buildShape(doc, *ea, sa, 0);
    buildShape(doc, *eb, sb, 0);
    if (sa.empty() || sb.empty()) {
        res.error = QStringLiteral("entity #%1 has no measurable geometry")
                        .arg(sa.empty() ? aId : bId);
        return res;
    }
    if (sa.approx)
        res.notes << QStringLiteral("#%1 measured on its bounding box").arg(aId);
    if (sb.approx)
        res.notes << QStringLiteral("#%1 measured on its bounding box").arg(bId);
    res.exact = !sa.approx && !sb.approx;

    // Expand polygon boundaries into capsules once.
    std::vector<Capsule> capsA = sa.capsules;
    std::vector<Capsule> capsB = sb.capsules;
    for (const PolySet& ps : sa.polys)
        appendRingCapsules(ps, capsA);
    for (const PolySet& ps : sb.polys)
        appendRingCapsules(ps, capsB);

    Best best;
    for (const Capsule& ca : capsA)
        for (const Capsule& cb : capsB)
            capsuleVsCapsule(best, ca, cb);
    for (const Disk& da : sa.disks) {
        for (const Capsule& cb : capsB)
            diskVsCapsule(best, da, cb, /*diskIsA=*/true);
        for (const Disk& db : sb.disks)
            diskVsDisk(best, da, db);
    }
    for (const Disk& db : sb.disks)
        for (const Capsule& ca : capsA)
            diskVsCapsule(best, db, ca, /*diskIsA=*/false);

    // "Entirely inside a filled polygon" leaves the boundary distance
    // positive — catch it explicitly (drill inside its pad, pad in a pour).
    Vec2d where;
    if (best.d > 0.0) {
        if (containedInPolys(sa, sb, where) || containedInPolys(sb, sa, where)) {
            best.d = 0.0;
            best.pa = best.pb = where;
        }
    }

    // Deep overlap: the offset edge points crossed each other — collapse to
    // one representative contact point inside the shared material.
    if (best.d < 0.0)
        best.pa = best.pb = (best.pa + best.pb) * 0.5;

    res.ok = true;
    res.overlap = best.d <= 1e-9;
    res.distance = std::max(0.0, best.d);
    res.pa = best.pa;
    res.pb = best.pb;
    return res;
}

} // namespace measure
} // namespace viki
