#include "Regions.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
#include <vector>

#include "GeomUtil.h"
#include "Intersect.h"

namespace viki {

namespace {

// ---- Curve flattening ------------------------------------------------------

// Flatten a curve into a chain of world points (includes both ends). A closed
// circle produces a chain whose last point equals its first.
std::vector<Vec2d> flatten(const Curve& c, double chordTol)
{
    std::vector<Vec2d> pts;
    if (!c.isArc) {
        pts.push_back(c.a);
        pts.push_back(c.b);
        return pts;
    }
    int segments = 16;
    if (chordTol > 0 && chordTol < c.radius) {
        const double step = 2.0 * std::acos(1.0 - chordTol / c.radius);
        segments = std::max(2, int(std::ceil(std::fabs(c.sweep) / step)));
    }
    segments = std::min(segments, 4096);
    pts.reserve(size_t(segments) + 1);
    for (int i = 0; i <= segments; ++i) {
        const double a = c.startAngle + c.sweep * (double(i) / segments);
        pts.push_back(c.center + Vec2d::polar(c.radius, a));
    }
    return pts;
}

// ---- Pairwise intersections of the ORIGINAL (exact) curves -----------------

void curveCurveIntersections(const Curve& x, const Curve& y, std::vector<Vec2d>& out)
{
    if (!x.isArc && !y.isArc) {
        intersectSegSeg(x.a, x.b, y.a, y.b, out);
    } else if (!x.isArc && y.isArc) {
        intersectSegArc(x.a, x.b, y.center, y.radius, y.startAngle, y.sweep, out);
    } else if (x.isArc && !y.isArc) {
        intersectSegArc(y.a, y.b, x.center, x.radius, x.startAngle, x.sweep, out);
    } else {
        intersectArcArc(x.center, x.radius, x.startAngle, x.sweep, y.center, y.radius,
                        y.startAngle, y.sweep, out);
    }
}

// Parameter of `p`'s projection along a curve, monotonic in traversal order,
// used only to sort split points along the curve. For lines: fraction along
// a->b. For arcs: CCW angular offset from startAngle.
double paramAlong(const Curve& c, const Vec2d& p)
{
    if (!c.isArc) {
        const Vec2d d = c.b - c.a;
        const double lenSq = d.lengthSq();
        if (nearZero(lenSq))
            return 0.0;
        return (p - c.a).dot(d) / lenSq;
    }
    return normalizeAngle((p - c.center).angle() - c.startAngle);
}

// ---- Planar graph ----------------------------------------------------------

// Welds points to integer node ids within `weldTol`.
class NodeSet {
public:
    explicit NodeSet(double weldTol) : m_tol(weldTol), m_cell(std::max(weldTol, 1e-12) * 4.0) {}

    int add(const Vec2d& p)
    {
        // Search the 3x3 neighborhood of hash cells for an existing node.
        const long cx = long(std::floor(p.x / m_cell));
        const long cy = long(std::floor(p.y / m_cell));
        for (long dx = -1; dx <= 1; ++dx)
            for (long dy = -1; dy <= 1; ++dy) {
                auto it = m_grid.find(key(cx + dx, cy + dy));
                if (it == m_grid.end())
                    continue;
                for (int id : it->second)
                    if ((m_pts[size_t(id)] - p).lengthSq() <= m_tol * m_tol)
                        return id;
            }
        const int id = int(m_pts.size());
        m_pts.push_back(p);
        m_grid[key(cx, cy)].push_back(id);
        return id;
    }

    const std::vector<Vec2d>& points() const { return m_pts; }

private:
    static long long key(long x, long y)
    {
        // Shift in the unsigned domain: left-shifting a negative signed value
        // is undefined behavior (caught by UBSan for cells at x < 0).
        const auto ux = static_cast<unsigned long long>(static_cast<long long>(x));
        const auto uy = static_cast<unsigned long long>(static_cast<long long>(y));
        return static_cast<long long>((ux << 32) ^ (uy & 0xffffffffULL));
    }
    double m_tol;
    double m_cell;
    std::vector<Vec2d> m_pts;
    std::unordered_map<long long, std::vector<int>> m_grid;
};

} // namespace

std::vector<Region> findRegions(const std::vector<Curve>& curves, double chordTol, double weldTol)
{
    std::vector<Region> result;
    if (curves.size() < 2)
        return result;

    // 1. All pairwise intersection points, indexed per curve.
    std::vector<std::vector<Vec2d>> splitPts(curves.size());
    for (size_t i = 0; i < curves.size(); ++i)
        for (size_t j = i + 1; j < curves.size(); ++j) {
            std::vector<Vec2d> pts;
            curveCurveIntersections(curves[i], curves[j], pts);
            for (const Vec2d& p : pts) {
                splitPts[i].push_back(p);
                splitPts[j].push_back(p);
            }
        }

    NodeSet nodes(weldTol);

    // Undirected edges as ordered node-id pairs, deduplicated.
    std::map<std::pair<int, int>, bool> edgeSet;
    auto addEdge = [&](int u, int v) {
        if (u == v)
            return;
        edgeSet[{std::min(u, v), std::max(u, v)}] = true;
    };

    // 2. Split each curve at its intersection points, flatten, and register
    //    edges. Split points are inserted into the flattened chain by nearest
    //    segment so arc geometry stays faithful.
    for (size_t i = 0; i < curves.size(); ++i) {
        const Curve& c = curves[i];
        std::vector<Vec2d> chain = flatten(c, chordTol);

        // Collect the (parameter-sorted) intersection points that lie strictly
        // between the chain ends, and stitch them into the chain.
        std::vector<Vec2d> splits = splitPts[i];
        // Sort split points along the curve.
        std::sort(splits.begin(), splits.end(),
                  [&](const Vec2d& p, const Vec2d& q) { return paramAlong(c, p) < paramAlong(c, q); });

        // Insert each split point into the chain right after the vertex whose
        // cumulative parameter it exceeds. For a robust-but-simple approach we
        // rebuild the chain by merging split points into the polyline by param.
        std::vector<std::pair<double, Vec2d>> merged;
        merged.reserve(chain.size() + splits.size());
        for (const Vec2d& p : chain)
            merged.push_back({paramAlong(c, p), p});
        for (const Vec2d& p : splits)
            merged.push_back({paramAlong(c, p), p});
        std::sort(merged.begin(), merged.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        int prev = -1;
        for (const auto& mv : merged) {
            const int id = nodes.add(mv.second);
            if (prev >= 0)
                addEdge(prev, id);
            prev = id;
        }
    }

    const std::vector<Vec2d>& P = nodes.points();
    if (edgeSet.empty())
        return result;

    // 3. Build directed half-edges. Each undirected edge gives two.
    struct HalfEdge {
        int from, to;
        double angle; // direction from->to
        bool used = false;
    };
    std::vector<HalfEdge> he;
    he.reserve(edgeSet.size() * 2);
    // Adjacency: for each node, indices of half-edges leaving it.
    std::vector<std::vector<int>> out(P.size());
    auto pushHalf = [&](int u, int v) {
        HalfEdge h;
        h.from = u;
        h.to = v;
        h.angle = (P[size_t(v)] - P[size_t(u)]).angle();
        const int idx = int(he.size());
        he.push_back(h);
        out[size_t(u)].push_back(idx);
    };
    for (const auto& kv : edgeSet) {
        pushHalf(kv.first.first, kv.first.second);
        pushHalf(kv.first.second, kv.first.first);
    }

    // For quick "next around vertex" lookup, sort each node's outgoing edges by
    // angle. We choose the next half-edge as the most-clockwise turn, which
    // walks minimal interior faces.
    for (auto& adj : out)
        std::sort(adj.begin(), adj.end(),
                  [&](int x, int y) { return he[size_t(x)].angle < he[size_t(y)].angle; });

    // Map (from,to) -> half-edge index, to find the reverse edge.
    std::map<std::pair<int, int>, int> heIndex;
    for (int i = 0; i < int(he.size()); ++i)
        heIndex[{he[size_t(i)].from, he[size_t(i)].to}] = i;

    auto nextHalfEdge = [&](int idx) -> int {
        // Arrive at node `to` via half-edge idx. The face lies to the LEFT
        // (CCW faces). Take the reverse edge (to->from) and pick the outgoing
        // edge that is the next one clockwise from it => most clockwise turn.
        const HalfEdge& h = he[size_t(idx)];
        const int rev = heIndex[{h.to, h.from}];
        const auto& adj = out[size_t(h.to)];
        // Find rev's position in the angle-sorted adjacency, step one CW (down).
        int pos = -1;
        for (int k = 0; k < int(adj.size()); ++k)
            if (adj[size_t(k)] == rev) {
                pos = k;
                break;
            }
        if (pos < 0)
            return -1;
        const int n = int(adj.size());
        const int nextPos = (pos - 1 + n) % n; // one step clockwise
        return adj[size_t(nextPos)];
    };

    auto signedArea = [&](const std::vector<int>& ring) {
        double s = 0.0;
        for (size_t k = 0; k < ring.size(); ++k) {
            const Vec2d& a = P[size_t(ring[k])];
            const Vec2d& b = P[size_t(ring[(k + 1) % ring.size()])];
            s += a.cross(b);
        }
        return 0.5 * s;
    };

    // 4. Trace all faces.
    for (int start = 0; start < int(he.size()); ++start) {
        if (he[size_t(start)].used)
            continue;
        std::vector<int> ringNodes;
        int cur = start;
        bool ok = true;
        for (size_t guard = 0; guard <= he.size() + 1; ++guard) {
            if (he[size_t(cur)].used && cur != start && !ringNodes.empty()) {
                ok = false;
                break;
            }
            he[size_t(cur)].used = true;
            ringNodes.push_back(he[size_t(cur)].from);
            const int nxt = nextHalfEdge(cur);
            if (nxt < 0) {
                ok = false;
                break;
            }
            if (nxt == start)
                break;
            cur = nxt;
        }
        if (!ok || ringNodes.size() < 3)
            continue;
        const double area = signedArea(ringNodes);
        // Interior minimal faces come out CCW (positive area) with this
        // most-clockwise-turn rule. The single outer face comes out CW
        // (negative area) and is dropped.
        if (area <= 1e-9)
            continue;
        Region reg;
        reg.area = area;
        reg.boundary.reserve(ringNodes.size());
        for (int id : ringNodes)
            reg.boundary.push_back(P[size_t(id)]);
        result.push_back(std::move(reg));
    }

    return result;
}

} // namespace viki
