#include "EntitiesEx.h"

#include <QJsonArray>

#include "Entities.h"
#include "geom/GeomUtil.h"

namespace viki {
namespace {

// Arc parameters of a bulged polyline segment a->b. bulge = tan(sweep/4).
struct BulgeArc {
    Vec2d center;
    double radius;
    double startAngle;
    double sweep; // signed: >0 CCW, <0 CW
};

std::optional<BulgeArc> bulgeToArc(const Vec2d& a, const Vec2d& b, double bulge)
{
    if (nearZero(bulge) || nearEqual(a, b))
        return std::nullopt;
    const double sweep = 4.0 * std::atan(bulge); // signed
    const double chord = a.distanceTo(b);
    const double radius = chord / (2.0 * std::fabs(std::sin(sweep / 2.0)));
    // Center is on the perpendicular bisector; offset sign follows the bulge.
    const Vec2d mid = (a + b) * 0.5;
    const double h = radius * std::cos(sweep / 2.0) * (bulge > 0 ? 1.0 : -1.0);
    const Vec2d center = mid + (b - a).normalized().perp() * h;
    const double startAngle = (a - center).angle();
    return BulgeArc{center, radius, startAngle, sweep};
}

void flattenBulgeSegment(const Vec2d& a, const Vec2d& b, double bulge, double tol,
                         std::vector<Vec2d>& out)
{
    // Appends the segment WITHOUT the leading vertex `a` (caller emits it).
    const auto arc = bulgeToArc(a, b, bulge);
    if (!arc) {
        out.push_back(b);
        return;
    }
    std::vector<Vec2d> pts;
    flattenArc(arc->center, arc->radius, arc->startAngle, arc->sweep, tol, pts);
    out.insert(out.end(), pts.begin() + 1, pts.end());
}

} // namespace

// ---- PolylineEntity ---------------------------------------------------------

std::unique_ptr<Entity> PolylineEntity::clone() const
{
    return std::make_unique<PolylineEntity>(*this);
}

BBox2d PolylineEntity::bounds() const
{
    BBox2d box;
    const size_t n = m_vertices.size();
    for (size_t i = 0; i < n; ++i) {
        box.expand(m_vertices[i].pos);
        // Bulged segments can overshoot the vertex hull: include arc box.
        const bool last = (i + 1 == n);
        if (last && !m_closed)
            break;
        const PolyVertex& v = m_vertices[i];
        const Vec2d next = m_vertices[(i + 1) % n].pos;
        if (const auto arc = bulgeToArc(v.pos, next, v.bulge)) {
            // Reuse ArcEntity's quadrant-aware box via a temporary.
            const double start = arc->sweep >= 0
                                     ? arc->startAngle
                                     : normalizeAngle(arc->startAngle + arc->sweep);
            ArcEntity tmp(arc->center, arc->radius, start, std::fabs(arc->sweep));
            box.expand(tmp.bounds());
        }
    }
    return box;
}

void PolylineEntity::transform(const Xform2d& xf)
{
    const bool mirrored = xf.det() < 0;
    for (PolyVertex& v : m_vertices) {
        v.pos = xf.apply(v.pos);
        if (mirrored)
            v.bulge = -v.bulge;
    }
}

void PolylineEntity::buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const
{
    if (m_vertices.size() < 2)
        return;
    StrokePrimitive s;
    s.rgb = ctx.resolvedColor;
    s.closed = false; // closure handled explicitly so the last bulge renders
    s.points.push_back(m_vertices.front().pos);
    const size_t n = m_vertices.size();
    for (size_t i = 0; i + 1 < n; ++i)
        flattenBulgeSegment(m_vertices[i].pos, m_vertices[i + 1].pos, m_vertices[i].bulge,
                            ctx.chordTolerance, s.points);
    if (m_closed)
        flattenBulgeSegment(m_vertices[n - 1].pos, m_vertices[0].pos, m_vertices[n - 1].bulge,
                            ctx.chordTolerance, s.points);
    out.strokes.push_back(std::move(s));
}

void PolylineEntity::geomToJson(QJsonObject& obj) const
{
    QJsonArray verts;
    for (const PolyVertex& v : m_vertices) {
        QJsonArray e{v.pos.x, v.pos.y};
        if (!nearZero(v.bulge))
            e.append(v.bulge);
        verts.append(e);
    }
    obj[QStringLiteral("vertices")] = verts;
    obj[QStringLiteral("closed")] = m_closed;
}

void PolylineEntity::geomFromJson(const QJsonObject& obj)
{
    m_vertices.clear();
    for (const QJsonValue& v : obj[QStringLiteral("vertices")].toArray()) {
        const QJsonArray e = v.toArray();
        PolyVertex vert;
        vert.pos = {e.at(0).toDouble(), e.at(1).toDouble()};
        vert.bulge = e.size() > 2 ? e.at(2).toDouble() : 0.0;
        m_vertices.push_back(vert);
    }
    m_closed = obj[QStringLiteral("closed")].toBool();
}

// ---- EllipseEntity ----------------------------------------------------------

std::unique_ptr<Entity> EllipseEntity::clone() const
{
    return std::make_unique<EllipseEntity>(*this);
}

BBox2d EllipseEntity::bounds() const
{
    // Sampled box (analytic ellipse extrema are fussy with arcs; 64 samples
    // is exact to ~0.1% which is plenty for culling/zoom).
    BBox2d box;
    const int kSamples = 64;
    for (int i = 0; i <= kSamples; ++i) {
        const double t = m_startParam + (m_endParam - m_startParam) * i / kSamples;
        box.expand(pointAt(t));
    }
    return box;
}

void EllipseEntity::transform(const Xform2d& xf)
{
    m_center = xf.apply(m_center);
    m_major = xf.applyVector(m_major);
    if (xf.det() < 0) {
        // Mirror flips the traversal direction; keep params meaningful.
        std::swap(m_startParam, m_endParam);
        m_startParam = -m_startParam;
        m_endParam = -m_endParam;
        m_ratio = -m_ratio; // minor axis flips relative to major.perp()
    }
    if (m_endParam <= m_startParam)
        m_endParam += 2.0 * M_PI;
}

void EllipseEntity::buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const
{
    StrokePrimitive s;
    s.rgb = ctx.resolvedColor;
    const double a = m_major.length();
    int segments = 32;
    if (ctx.chordTolerance > 0 && ctx.chordTolerance < a) {
        const double step = 2.0 * std::acos(1.0 - ctx.chordTolerance / a);
        segments = std::clamp(int(std::ceil((m_endParam - m_startParam) / step)), 8, 4096);
    }
    for (int i = 0; i <= segments; ++i) {
        const double t = m_startParam + (m_endParam - m_startParam) * i / segments;
        s.points.push_back(pointAt(t));
    }
    s.closed = isFull();
    out.strokes.push_back(std::move(s));
}

void EllipseEntity::geomToJson(QJsonObject& obj) const
{
    obj[QStringLiteral("center")] = pointToJson(m_center);
    obj[QStringLiteral("major")] = pointToJson(m_major);
    obj[QStringLiteral("ratio")] = m_ratio;
    obj[QStringLiteral("start_param")] = m_startParam;
    obj[QStringLiteral("end_param")] = m_endParam;
}

void EllipseEntity::geomFromJson(const QJsonObject& obj)
{
    m_center = pointFromJson(obj[QStringLiteral("center")]);
    m_major = pointFromJson(obj[QStringLiteral("major")]);
    m_ratio = obj[QStringLiteral("ratio")].toDouble(0.5);
    m_startParam = obj[QStringLiteral("start_param")].toDouble(0.0);
    m_endParam = obj[QStringLiteral("end_param")].toDouble(2.0 * M_PI);
}

// ---- SplineEntity -----------------------------------------------------------

std::unique_ptr<Entity> SplineEntity::clone() const
{
    return std::make_unique<SplineEntity>(*this);
}

Vec2d SplineEntity::evaluate(double u) const
{
    // De Boor with optional rational weights.
    const int p = degree;
    const int n = int(controlPoints.size()) - 1;
    if (n < p || knots.size() != controlPoints.size() + size_t(p) + 1)
        return controlPoints.empty() ? Vec2d{} : controlPoints.front();

    u = std::clamp(u, knots[size_t(p)], knots[size_t(n) + 1]);
    // Find knot span k with knots[k] <= u < knots[k+1].
    int k = p;
    for (int i = p; i <= n; ++i) {
        if (u >= knots[size_t(i)] && u < knots[size_t(i) + 1]) {
            k = i;
            break;
        }
        k = n; // u == last knot
    }

    const bool rational = weights.size() == controlPoints.size();
    struct H { double x, y, w; };
    std::vector<H> d(size_t(p) + 1);
    for (int j = 0; j <= p; ++j) {
        const Vec2d& c = controlPoints[size_t(k - p + j)];
        const double w = rational ? weights[size_t(k - p + j)] : 1.0;
        d[size_t(j)] = {c.x * w, c.y * w, w};
    }
    for (int r = 1; r <= p; ++r) {
        for (int j = p; j >= r; --j) {
            const size_t i = size_t(k - p + j);
            const double denom = knots[i + size_t(p) - size_t(r) + 1] - knots[i];
            const double alpha = nearZero(denom) ? 0.0 : (u - knots[i]) / denom;
            d[size_t(j)].x = (1 - alpha) * d[size_t(j) - 1].x + alpha * d[size_t(j)].x;
            d[size_t(j)].y = (1 - alpha) * d[size_t(j) - 1].y + alpha * d[size_t(j)].y;
            d[size_t(j)].w = (1 - alpha) * d[size_t(j) - 1].w + alpha * d[size_t(j)].w;
        }
    }
    const H& h = d[size_t(p)];
    return nearZero(h.w) ? Vec2d{h.x, h.y} : Vec2d{h.x / h.w, h.y / h.w};
}

BBox2d SplineEntity::bounds() const
{
    // Control polygon hull contains the curve (convex hull property).
    BBox2d box;
    for (const Vec2d& c : controlPoints)
        box.expand(c);
    for (const Vec2d& f : fitPoints)
        box.expand(f);
    return box;
}

void SplineEntity::transform(const Xform2d& xf)
{
    for (Vec2d& c : controlPoints)
        c = xf.apply(c);
    for (Vec2d& f : fitPoints)
        f = xf.apply(f);
}

void SplineEntity::buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const
{
    StrokePrimitive s;
    s.rgb = ctx.resolvedColor;
    const int p = degree;
    const int n = int(controlPoints.size()) - 1;
    if (n >= p && knots.size() == controlPoints.size() + size_t(p) + 1) {
        // Sample density scaled by control polygon length vs tolerance.
        double polyLen = 0;
        for (size_t i = 0; i + 1 < controlPoints.size(); ++i)
            polyLen += controlPoints[i].distanceTo(controlPoints[i + 1]);
        int samples = 64;
        if (ctx.chordTolerance > 0)
            samples = std::clamp(int(std::sqrt(polyLen / ctx.chordTolerance)) * 4, 16, 2048);
        const double u0 = knots[size_t(p)];
        const double u1 = knots[controlPoints.size()];
        for (int i = 0; i <= samples; ++i)
            s.points.push_back(evaluate(u0 + (u1 - u0) * i / samples));
    } else if (!fitPoints.empty()) {
        s.points = fitPoints; // honest fallback: chord through fit points
    } else {
        return;
    }
    s.closed = closed;
    out.strokes.push_back(std::move(s));
}

void SplineEntity::geomToJson(QJsonObject& obj) const
{
    obj[QStringLiteral("degree")] = degree;
    obj[QStringLiteral("closed")] = closed;
    QJsonArray ctrl, kn, w, fit;
    for (const Vec2d& c : controlPoints)
        ctrl.append(pointToJson(c));
    for (const double k : knots)
        kn.append(k);
    for (const double x : weights)
        w.append(x);
    for (const Vec2d& f : fitPoints)
        fit.append(pointToJson(f));
    obj[QStringLiteral("control")] = ctrl;
    obj[QStringLiteral("knots")] = kn;
    if (!weights.empty())
        obj[QStringLiteral("weights")] = w;
    if (!fitPoints.empty())
        obj[QStringLiteral("fit")] = fit;
}

void SplineEntity::geomFromJson(const QJsonObject& obj)
{
    degree = obj[QStringLiteral("degree")].toInt(3);
    closed = obj[QStringLiteral("closed")].toBool();
    controlPoints.clear();
    knots.clear();
    weights.clear();
    fitPoints.clear();
    for (const QJsonValue& v : obj[QStringLiteral("control")].toArray())
        controlPoints.push_back(pointFromJson(v));
    for (const QJsonValue& v : obj[QStringLiteral("knots")].toArray())
        knots.push_back(v.toDouble());
    for (const QJsonValue& v : obj[QStringLiteral("weights")].toArray())
        weights.push_back(v.toDouble());
    for (const QJsonValue& v : obj[QStringLiteral("fit")].toArray())
        fitPoints.push_back(pointFromJson(v));
}

// ---- PointEntity ------------------------------------------------------------

std::unique_ptr<Entity> PointEntity::clone() const
{
    return std::make_unique<PointEntity>(*this);
}

void PointEntity::buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const
{
    // Screen-constant cross marker (~8 px).
    const double r = 4.0 * ctx.pixelSize;
    StrokePrimitive h, v;
    h.rgb = v.rgb = ctx.resolvedColor;
    h.points = {m_pos - Vec2d{r, 0}, m_pos + Vec2d{r, 0}};
    v.points = {m_pos - Vec2d{0, r}, m_pos + Vec2d{0, r}};
    out.strokes.push_back(std::move(h));
    out.strokes.push_back(std::move(v));
}

void PointEntity::geomToJson(QJsonObject& obj) const
{
    obj[QStringLiteral("pos")] = pointToJson(m_pos);
}

void PointEntity::geomFromJson(const QJsonObject& obj)
{
    m_pos = pointFromJson(obj[QStringLiteral("pos")]);
}

// ---- XLineEntity ------------------------------------------------------------

std::unique_ptr<Entity> XLineEntity::clone() const
{
    return std::make_unique<XLineEntity>(*this);
}

void XLineEntity::buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const
{
    // Clip to the view box (or a generous fallback span).
    double span = 2e6;
    Vec2d ref = m_base;
    if (ctx.viewBox.isValid()) {
        ref = ctx.viewBox.center();
        span = 2.0 * std::max(ctx.viewBox.width(), ctx.viewBox.height()) + 1.0;
    }
    // Project the reference onto the line, extend both ways.
    const Vec2d foot = m_base + m_dir * (ref - m_base).dot(m_dir);
    StrokePrimitive s;
    s.rgb = ctx.resolvedColor;
    s.points = {foot - m_dir * span, foot + m_dir * span};
    out.strokes.push_back(std::move(s));
}

void XLineEntity::geomToJson(QJsonObject& obj) const
{
    obj[QStringLiteral("base")] = pointToJson(m_base);
    obj[QStringLiteral("dir")] = pointToJson(m_dir);
}

void XLineEntity::geomFromJson(const QJsonObject& obj)
{
    m_base = pointFromJson(obj[QStringLiteral("base")]);
    m_dir = pointFromJson(obj[QStringLiteral("dir")]).normalized();
}


// ---- snap candidates --------------------------------------------------------

void PolylineEntity::snapPoints(std::vector<SnapPoint>& out) const
{
    const size_t n = m_vertices.size();
    for (size_t i = 0; i < n; ++i) {
        out.push_back({m_vertices[i].pos, SnapKind::Endpoint});
        const bool last = (i + 1 == n);
        if (last && !m_closed)
            break;
        const Vec2d a = m_vertices[i].pos;
        const Vec2d b = m_vertices[(i + 1) % n].pos;
        if (const auto arc = bulgeToArc(a, b, m_vertices[i].bulge)) {
            out.push_back({arc->center + Vec2d::polar(arc->radius,
                               arc->startAngle + arc->sweep * 0.5),
                           SnapKind::Midpoint});
            out.push_back({arc->center, SnapKind::Center});
        } else {
            out.push_back({(a + b) * 0.5, SnapKind::Midpoint});
        }
    }
}

void EllipseEntity::snapPoints(std::vector<SnapPoint>& out) const
{
    out.push_back({m_center, SnapKind::Center});
    for (int q = 0; q < 4; ++q) {
        const double t = q * M_PI_2;
        out.push_back({pointAt(t), SnapKind::Quadrant});
    }
    if (!isFull()) {
        out.push_back({pointAt(m_startParam), SnapKind::Endpoint});
        out.push_back({pointAt(m_endParam), SnapKind::Endpoint});
    }
}

void SplineEntity::snapPoints(std::vector<SnapPoint>& out) const
{
    if (!fitPoints.empty()) {
        out.push_back({fitPoints.front(), SnapKind::Endpoint});
        out.push_back({fitPoints.back(), SnapKind::Endpoint});
        return;
    }
    if (!controlPoints.empty() && knots.size() == controlPoints.size() + size_t(degree) + 1) {
        out.push_back({evaluate(knots[size_t(degree)]), SnapKind::Endpoint});
        out.push_back({evaluate(knots[controlPoints.size()]), SnapKind::Endpoint});
    }
}

void PointEntity::snapPoints(std::vector<SnapPoint>& out) const
{
    out.push_back({m_pos, SnapKind::Endpoint});
}


// ---- stretch & grips ---------------------------------------------------------

void PolylineEntity::stretch(const BBox2d& window, const Vec2d& delta)
{
    for (PolyVertex& v : m_vertices)
        if (window.contains(v.pos))
            v.pos += delta;
}

std::vector<Vec2d> PolylineEntity::gripPoints() const
{
    std::vector<Vec2d> out;
    for (const PolyVertex& v : m_vertices)
        out.push_back(v.pos);
    return out;
}

void PolylineEntity::moveGrip(int index, const Vec2d& to)
{
    if (index >= 0 && index < int(m_vertices.size()))
        m_vertices[size_t(index)].pos = to;
}

void SplineEntity::stretch(const BBox2d& window, const Vec2d& delta)
{
    for (Vec2d& c : controlPoints)
        if (window.contains(c))
            c += delta;
    for (Vec2d& f : fitPoints)
        if (window.contains(f))
            f += delta;
}

} // namespace viki
