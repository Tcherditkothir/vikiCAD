#include "Entities.h"

#include <QJsonArray>

#include "geom/GeomUtil.h"

namespace viki {

void flattenArc(const Vec2d& center, double radius, double startAngle, double sweep,
                double chordTolerance, std::vector<Vec2d>& out)
{
    // Segment count from max chord deviation: sagitta = r(1-cos(step/2)) <= tol.
    int segments = 8;
    if (chordTolerance > 0 && chordTolerance < radius) {
        const double step = 2.0 * std::acos(1.0 - chordTolerance / radius);
        segments = std::max(2, int(std::ceil(std::fabs(sweep) / step)));
    }
    segments = std::min(segments, 4096);

    out.reserve(out.size() + size_t(segments) + 1);
    for (int i = 0; i <= segments; ++i) {
        const double a = startAngle + sweep * (double(i) / segments);
        out.push_back(center + Vec2d::polar(radius, a));
    }
}

// ---- LineEntity ------------------------------------------------------------

std::unique_ptr<Entity> LineEntity::clone() const
{
    return std::make_unique<LineEntity>(*this);
}

void LineEntity::transform(const Xform2d& xf)
{
    m_p1 = xf.apply(m_p1);
    m_p2 = xf.apply(m_p2);
}

void LineEntity::buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const
{
    StrokePrimitive s;
    s.rgb = ctx.resolvedColor;
    s.points = {m_p1, m_p2};
    out.strokes.push_back(std::move(s));
}

void LineEntity::geomToJson(QJsonObject& obj) const
{
    obj[QStringLiteral("p1")] = pointToJson(m_p1);
    obj[QStringLiteral("p2")] = pointToJson(m_p2);
}

void LineEntity::geomFromJson(const QJsonObject& obj)
{
    m_p1 = pointFromJson(obj[QStringLiteral("p1")]);
    m_p2 = pointFromJson(obj[QStringLiteral("p2")]);
}

// ---- CircleEntity ----------------------------------------------------------

std::unique_ptr<Entity> CircleEntity::clone() const
{
    return std::make_unique<CircleEntity>(*this);
}

void CircleEntity::transform(const Xform2d& xf)
{
    m_center = xf.apply(m_center);
    m_radius *= xf.uniformScale();
}

void CircleEntity::buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const
{
    StrokePrimitive s;
    s.rgb = ctx.resolvedColor;
    s.closed = true;
    flattenArc(m_center, m_radius, 0.0, 2.0 * M_PI, ctx.chordTolerance, s.points);
    out.strokes.push_back(std::move(s));
}

void CircleEntity::geomToJson(QJsonObject& obj) const
{
    obj[QStringLiteral("center")] = pointToJson(m_center);
    obj[QStringLiteral("radius")] = m_radius;
}

void CircleEntity::geomFromJson(const QJsonObject& obj)
{
    m_center = pointFromJson(obj[QStringLiteral("center")]);
    m_radius = obj[QStringLiteral("radius")].toDouble(1.0);
}

// ---- ArcEntity -------------------------------------------------------------

std::unique_ptr<Entity> ArcEntity::clone() const
{
    return std::make_unique<ArcEntity>(*this);
}

BBox2d ArcEntity::bounds() const
{
    BBox2d box{startPoint(), endPoint()};
    // Extend by every quadrant point the sweep crosses.
    for (int q = 0; q < 4; ++q) {
        const double a = q * M_PI_2;
        if (angleOnArc(a, m_startAngle, m_sweep))
            box.expand(m_center + Vec2d::polar(m_radius, a));
    }
    return box;
}

void ArcEntity::transform(const Xform2d& xf)
{
    const double rot = std::atan2(xf.b, xf.a);
    m_center = xf.apply(m_center);
    m_radius *= xf.uniformScale();
    if (xf.det() >= 0) {
        m_startAngle = normalizeAngle(m_startAngle + rot);
    } else {
        // Reflection maps direction angle t to (rot - t); the CCW span
        // [start, start+sweep] becomes [rot-(start+sweep), rot-start],
        // still CCW with the same sweep.
        m_startAngle = normalizeAngle(rot - (m_startAngle + m_sweep));
    }
}

void ArcEntity::buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const
{
    StrokePrimitive s;
    s.rgb = ctx.resolvedColor;
    flattenArc(m_center, m_radius, m_startAngle, m_sweep, ctx.chordTolerance, s.points);
    out.strokes.push_back(std::move(s));
}

void ArcEntity::geomToJson(QJsonObject& obj) const
{
    obj[QStringLiteral("center")] = pointToJson(m_center);
    obj[QStringLiteral("radius")] = m_radius;
    obj[QStringLiteral("start_angle")] = m_startAngle;
    obj[QStringLiteral("sweep")] = m_sweep;
}

void ArcEntity::geomFromJson(const QJsonObject& obj)
{
    m_center = pointFromJson(obj[QStringLiteral("center")]);
    m_radius = obj[QStringLiteral("radius")].toDouble(1.0);
    m_startAngle = obj[QStringLiteral("start_angle")].toDouble(0.0);
    m_sweep = obj[QStringLiteral("sweep")].toDouble(M_PI);
}


// ---- snap candidates --------------------------------------------------------

void LineEntity::snapPoints(std::vector<SnapPoint>& out) const
{
    out.push_back({m_p1, SnapKind::Endpoint});
    out.push_back({m_p2, SnapKind::Endpoint});
    out.push_back({(m_p1 + m_p2) * 0.5, SnapKind::Midpoint});
}

void CircleEntity::snapPoints(std::vector<SnapPoint>& out) const
{
    out.push_back({m_center, SnapKind::Center});
    out.push_back({m_center + Vec2d{m_radius, 0}, SnapKind::Quadrant});
    out.push_back({m_center + Vec2d{0, m_radius}, SnapKind::Quadrant});
    out.push_back({m_center - Vec2d{m_radius, 0}, SnapKind::Quadrant});
    out.push_back({m_center - Vec2d{0, m_radius}, SnapKind::Quadrant});
}

void ArcEntity::snapPoints(std::vector<SnapPoint>& out) const
{
    out.push_back({startPoint(), SnapKind::Endpoint});
    out.push_back({endPoint(), SnapKind::Endpoint});
    out.push_back({m_center + Vec2d::polar(m_radius, m_startAngle + m_sweep * 0.5),
                   SnapKind::Midpoint});
    out.push_back({m_center, SnapKind::Center});
    for (int q = 0; q < 4; ++q) {
        const double a = q * M_PI_2;
        if (angleOnArc(a, m_startAngle, m_sweep))
            out.push_back({m_center + Vec2d::polar(m_radius, a), SnapKind::Quadrant});
    }
}

} // namespace viki
