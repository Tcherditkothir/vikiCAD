#include "Annotations.h"

#include <QJsonArray>

#include "Document.h"
#include "Entities.h"
#include "geom/GeomUtil.h"
#include "geom/Intersect.h"

namespace viki {
namespace {

const DimStyle kDefaultStyle;

const DimStyle& styleFor(const RenderContext& ctx, const QString& name)
{
    return ctx.doc ? ctx.doc->dimStyle(name) : kDefaultStyle;
}

QString formatLength(double mm, const RenderContext& ctx, const DimStyle& style)
{
    const bool inches =
        ctx.doc && ctx.doc->displayUnits() == DisplayUnits::Inches;
    const double v = inches ? mm / 25.4 : mm;
    return QString::number(v, 'f', style.decimals) + style.suffix;
}

} // namespace

void emitArrow(const Vec2d& tip, const Vec2d& dir, double size, uint32_t rgb,
               PrimitiveList& out)
{
    const Vec2d d = dir.normalized();
    const Vec2d back = tip - d * size;
    const Vec2d half = d.perp() * (size / 3.0);
    StrokePrimitive s;
    s.rgb = rgb;
    s.closed = true;
    s.filled = true;
    s.points = {tip, back + half, back - half};
    out.strokes.push_back(std::move(s));
}

// ---- TextEntity ---------------------------------------------------------------

std::unique_ptr<Entity> TextEntity::clone() const
{
    return std::make_unique<TextEntity>(*this);
}

BBox2d TextEntity::bounds() const
{
    const QStringList lines = m_text.split(QLatin1Char('\n'));
    double maxLen = 1;
    for (const QString& l : lines)
        maxLen = std::max(maxLen, double(l.size()));
    const double w = maxLen * kCharAspect * m_height;
    const double h = lines.size() * kLineSpacing * m_height;
    // Corners of the unrotated box (baseline at pos, lines go down).
    double x0 = 0;
    if (hAlign == TextHAlign::Center)
        x0 = -w / 2;
    else if (hAlign == TextHAlign::Right)
        x0 = -w;
    const Vec2d corners[4] = {{x0, m_height}, {x0 + w, m_height},
                              {x0, m_height - h}, {x0 + w, m_height - h}};
    BBox2d box;
    for (const Vec2d& corner : corners)
        box.expand(m_pos + corner.rotated(m_rotation));
    return box;
}

void TextEntity::transform(const Xform2d& xf)
{
    m_pos = xf.apply(m_pos);
    m_height *= xf.uniformScale();
    const double rot = std::atan2(xf.b, xf.a);
    m_rotation = xf.det() >= 0 ? m_rotation + rot : rot - m_rotation;
}

void TextEntity::buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const
{
    const QStringList lines = m_text.split(QLatin1Char('\n'));
    const Vec2d down = Vec2d{0, -kLineSpacing * m_height}.rotated(m_rotation);
    for (int i = 0; i < lines.size(); ++i) {
        TextPrimitive t;
        t.pos = m_pos + down * double(i);
        t.height = m_height;
        t.rotation = m_rotation;
        t.text = lines[i];
        t.rgb = ctx.resolvedColor;
        t.hAlign = hAlign;
        out.texts.push_back(std::move(t));
    }
    // Pick box for hit-testing only — never rendered.
    if (ctx.forHitTest) {
        StrokePrimitive anchor;
        anchor.rgb = ctx.resolvedColor;
        const BBox2d box = bounds();
        anchor.points = {box.min, {box.max.x, box.min.y}, box.max, {box.min.x, box.max.y}};
        anchor.closed = true;
        out.strokes.push_back(std::move(anchor));
    }
}

void TextEntity::snapPoints(std::vector<SnapPoint>& out) const
{
    out.push_back({m_pos, SnapKind::Endpoint});
}

void TextEntity::geomToJson(QJsonObject& obj) const
{
    obj[QStringLiteral("pos")] = pointToJson(m_pos);
    obj[QStringLiteral("height")] = m_height;
    obj[QStringLiteral("rotation")] = m_rotation;
    obj[QStringLiteral("text")] = m_text;
    obj[QStringLiteral("halign")] = int(hAlign);
}

void TextEntity::geomFromJson(const QJsonObject& obj)
{
    m_pos = pointFromJson(obj[QStringLiteral("pos")]);
    m_height = obj[QStringLiteral("height")].toDouble(3.5);
    m_rotation = obj[QStringLiteral("rotation")].toDouble(0.0);
    m_text = obj[QStringLiteral("text")].toString();
    hAlign = TextHAlign(obj[QStringLiteral("halign")].toInt(0));
}

// ---- DimensionEntity ------------------------------------------------------------

std::unique_ptr<Entity> DimensionEntity::clone() const
{
    return std::make_unique<DimensionEntity>(*this);
}

double DimensionEntity::measurement() const
{
    switch (kind) {
    case Kind::Linear:
        return std::fabs((b - a).dot(axis));
    case Kind::Aligned:
        return a.distanceTo(b);
    case Kind::Angular: {
        const double a1 = (b - a).angle();
        const double a2 = (c - a).angle();
        const double ap = (pos - a).angle();
        double sweep = ccwSweep(a1, a2);
        if (!angleOnArc(ap, a1, sweep))
            sweep = 2.0 * M_PI - sweep;
        return sweep;
    }
    case Kind::Radius:
        return a.distanceTo(b);
    case Kind::Diameter:
        return 2.0 * a.distanceTo(b);
    }
    return 0;
}

BBox2d DimensionEntity::bounds() const
{
    BBox2d box{a, b};
    box.expand(pos);
    if (kind == Kind::Angular)
        box.expand(c);
    return box.inflated(5.0); // arrows/text margin
}

void DimensionEntity::transform(const Xform2d& xf)
{
    a = xf.apply(a);
    b = xf.apply(b);
    c = xf.apply(c);
    pos = xf.apply(pos);
    if (kind == Kind::Linear)
        axis = xf.applyVector(axis).normalized();
}

void DimensionEntity::buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const
{
    const DimStyle& st = styleFor(ctx, style);
    const uint32_t rgb = ctx.resolvedColor;

    const auto pushLine = [&](const Vec2d& p, const Vec2d& q) {
        StrokePrimitive s;
        s.rgb = rgb;
        s.points = {p, q};
        out.strokes.push_back(std::move(s));
    };
    const auto pushText = [&](const Vec2d& at, double rot, const QString& str) {
        TextPrimitive t;
        t.pos = at;
        t.height = st.textHeight;
        // Keep text readable (never upside down).
        double r = normalizeAngle(rot);
        if (r > M_PI_2 && r <= 1.5 * M_PI)
            r = normalizeAngle(r + M_PI);
        t.rotation = r;
        t.text = str;
        t.rgb = rgb;
        t.hAlign = TextHAlign::Center;
        out.texts.push_back(std::move(t));
    };
    const QString label =
        !textOverride.isEmpty()
            ? textOverride
            : (kind == Kind::Angular
                   ? QString::number(measurement() * 180.0 / M_PI, 'f', 1) +
                         QStringLiteral("°")
                   : (kind == Kind::Radius ? QStringLiteral("R") :
                      kind == Kind::Diameter ? QStringLiteral("Ø") : QString()) +
                         formatLength(measurement(), ctx, st));

    switch (kind) {
    case Kind::Linear:
    case Kind::Aligned: {
        const Vec2d u = kind == Kind::Linear ? axis : (b - a).normalized();
        Vec2d n = u.perp();
        if ((pos - a).dot(n) < 0)
            n = n * -1.0;
        const double da = (pos - a).dot(n);
        const double db = (pos - b).dot(n);
        const Vec2d pa = a + n * da;
        const Vec2d pb = b + n * db;
        // Extension lines.
        pushLine(a + n * std::copysign(st.extOffset, da), pa + n * std::copysign(st.extBeyond, da));
        pushLine(b + n * std::copysign(st.extOffset, db), pb + n * std::copysign(st.extBeyond, db));
        // Dimension line + arrows.
        pushLine(pa, pb);
        const Vec2d dir = (pb - pa).normalized();
        emitArrow(pa, dir * -1.0, st.arrowSize, rgb, out);
        emitArrow(pb, dir, st.arrowSize, rgb, out);
        pushText((pa + pb) * 0.5 + n * (st.textGap + st.textHeight * 0.5), dir.angle(),
                 label);
        break;
    }
    case Kind::Angular: {
        const double r = std::max(a.distanceTo(pos), st.textHeight);
        const double a1 = (b - a).angle();
        const double a2 = (c - a).angle();
        double start = a1, sweep = ccwSweep(a1, a2);
        if (!angleOnArc((pos - a).angle(), a1, sweep)) {
            start = a2;
            sweep = 2.0 * M_PI - sweep;
        }
        StrokePrimitive arc;
        arc.rgb = rgb;
        flattenArc(a, r, start, sweep, ctx.chordTolerance, arc.points);
        if (arc.points.size() >= 2) {
            const Vec2d pStart = arc.points.front();
            const Vec2d pEnd = arc.points.back();
            emitArrow(pStart, Vec2d::polar(1.0, start + M_PI_2) * -1.0, st.arrowSize, rgb, out);
            emitArrow(pEnd, Vec2d::polar(1.0, start + sweep + M_PI_2), st.arrowSize, rgb, out);
            out.strokes.push_back(std::move(arc));
        }
        // Leg lines out to the arc.
        pushLine(a, a + Vec2d::polar(r, a1));
        pushLine(a, a + Vec2d::polar(r, a2));
        const double mid = start + sweep / 2.0;
        pushText(a + Vec2d::polar(r + st.textGap + st.textHeight * 0.5, mid),
                 mid + M_PI_2, label);
        break;
    }
    case Kind::Radius:
    case Kind::Diameter: {
        const double r = a.distanceTo(b);
        const Vec2d dir = (pos - a).lengthSq() > kGeomTol ? (pos - a).normalized()
                                                          : Vec2d{1, 0};
        const Vec2d onCurve = a + dir * r;
        pushLine(onCurve, pos);
        emitArrow(onCurve, (onCurve - pos).normalized(), st.arrowSize, rgb, out);
        TextPrimitive t;
        t.pos = pos + dir * st.textGap;
        t.height = st.textHeight;
        t.rotation = 0;
        t.text = label;
        t.rgb = rgb;
        t.hAlign = dir.x >= 0 ? TextHAlign::Left : TextHAlign::Right;
        out.texts.push_back(std::move(t));
        break;
    }
    }
}

void DimensionEntity::snapPoints(std::vector<SnapPoint>& out) const
{
    out.push_back({a, SnapKind::Endpoint});
    out.push_back({b, SnapKind::Endpoint});
    if (kind == Kind::Angular)
        out.push_back({c, SnapKind::Endpoint});
}

void DimensionEntity::geomToJson(QJsonObject& obj) const
{
    obj[QStringLiteral("kind")] = int(kind);
    obj[QStringLiteral("a")] = pointToJson(a);
    obj[QStringLiteral("b")] = pointToJson(b);
    obj[QStringLiteral("c")] = pointToJson(c);
    obj[QStringLiteral("pos")] = pointToJson(pos);
    obj[QStringLiteral("axis")] = pointToJson(axis);
    obj[QStringLiteral("dimstyle")] = style;
    if (!textOverride.isEmpty())
        obj[QStringLiteral("text_override")] = textOverride;
}

void DimensionEntity::geomFromJson(const QJsonObject& obj)
{
    kind = Kind(obj[QStringLiteral("kind")].toInt(0));
    a = pointFromJson(obj[QStringLiteral("a")]);
    b = pointFromJson(obj[QStringLiteral("b")]);
    c = pointFromJson(obj[QStringLiteral("c")]);
    pos = pointFromJson(obj[QStringLiteral("pos")]);
    axis = pointFromJson(obj[QStringLiteral("axis")]);
    if (axis.lengthSq() < kGeomTol)
        axis = {1, 0};
    style = obj[QStringLiteral("dimstyle")].toString(QStringLiteral("Standard"));
    textOverride = obj[QStringLiteral("text_override")].toString();
}

// ---- LeaderEntity ---------------------------------------------------------------

std::unique_ptr<Entity> LeaderEntity::clone() const
{
    return std::make_unique<LeaderEntity>(*this);
}

BBox2d LeaderEntity::bounds() const
{
    BBox2d box;
    for (const Vec2d& p : points)
        box.expand(p);
    return box.inflated(3.0);
}

void LeaderEntity::transform(const Xform2d& xf)
{
    for (Vec2d& p : points)
        p = xf.apply(p);
}

void LeaderEntity::buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const
{
    if (points.size() < 2)
        return;
    const DimStyle& st = styleFor(ctx, style);
    StrokePrimitive s;
    s.rgb = ctx.resolvedColor;
    s.points = points;
    out.strokes.push_back(std::move(s));
    emitArrow(points.front(), (points.front() - points[1]).normalized(), st.arrowSize,
              ctx.resolvedColor, out);
    if (!text.isEmpty()) {
        const Vec2d dir = (points.back() - points[points.size() - 2]).normalized();
        TextPrimitive t;
        t.pos = points.back() + Vec2d{dir.x >= 0 ? st.textGap : -st.textGap, 0};
        t.height = st.textHeight;
        t.text = text;
        t.rgb = ctx.resolvedColor;
        t.hAlign = dir.x >= 0 ? TextHAlign::Left : TextHAlign::Right;
        out.texts.push_back(std::move(t));
    }
}

void LeaderEntity::snapPoints(std::vector<SnapPoint>& out) const
{
    for (const Vec2d& p : points)
        out.push_back({p, SnapKind::Endpoint});
}

void LeaderEntity::geomToJson(QJsonObject& obj) const
{
    QJsonArray pts;
    for (const Vec2d& p : points)
        pts.append(pointToJson(p));
    obj[QStringLiteral("points")] = pts;
    obj[QStringLiteral("text")] = text;
    obj[QStringLiteral("dimstyle")] = style;
}

void LeaderEntity::geomFromJson(const QJsonObject& obj)
{
    points.clear();
    for (const QJsonValue& v : obj[QStringLiteral("points")].toArray())
        points.push_back(pointFromJson(v));
    text = obj[QStringLiteral("text")].toString();
    style = obj[QStringLiteral("dimstyle")].toString(QStringLiteral("Standard"));
}

// ---- HatchEntity ----------------------------------------------------------------

std::unique_ptr<Entity> HatchEntity::clone() const
{
    return std::make_unique<HatchEntity>(*this);
}

BBox2d HatchEntity::bounds() const
{
    BBox2d box;
    for (const auto& ring : rings)
        for (const Vec2d& p : ring)
            box.expand(p);
    return box;
}

void HatchEntity::transform(const Xform2d& xf)
{
    for (auto& ring : rings)
        for (Vec2d& p : ring)
            p = xf.apply(p);
    scale *= xf.uniformScale();
    angle += std::atan2(xf.b, xf.a) * (xf.det() >= 0 ? 1.0 : -1.0);
}

void HatchEntity::buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const
{
    if (rings.empty())
        return;

    if (pattern.compare(QLatin1String("SOLID"), Qt::CaseInsensitive) == 0) {
        // v1: rings filled individually (no hole subtraction yet).
        for (const auto& ring : rings) {
            StrokePrimitive s;
            s.rgb = ctx.resolvedColor;
            s.points = ring;
            s.closed = true;
            s.filled = true;
            out.strokes.push_back(std::move(s));
        }
        return;
    }

    // Line families: {angle offset, spacing in mm at scale 1}.
    std::vector<std::pair<double, double>> families;
    if (pattern.compare(QLatin1String("ANSI37"), Qt::CaseInsensitive) == 0) {
        families = {{M_PI_4, 3.175}, {3.0 * M_PI_4, 3.175}};
    } else { // ANSI31 and anything unknown
        families = {{M_PI_4, 3.175}};
    }

    // Boundary outline is drawn too (thin), so an empty pattern still shows.
    for (const auto& ring : rings) {
        StrokePrimitive s;
        s.rgb = ctx.resolvedColor;
        s.points = ring;
        s.closed = true;
        out.strokes.push_back(std::move(s));
    }

    for (const auto& [famAngle, famSpacing] : families) {
        const double theta = famAngle + angle;
        const double spacing = std::max(famSpacing * scale, 0.01);
        // Rotate boundary into pattern space (lines become horizontal).
        std::vector<std::pair<Vec2d, Vec2d>> segs;
        BBox2d box;
        for (const auto& ring : rings) {
            const size_t n = ring.size();
            for (size_t i = 0; i < n; ++i) {
                const Vec2d p = ring[i].rotated(-theta);
                const Vec2d q = ring[(i + 1) % n].rotated(-theta);
                segs.push_back({p, q});
                box.expand(p);
            }
        }
        if (!box.isValid())
            continue;
        const int kMaxLines = 5000; // guard against absurd densities
        int lineCount = 0;
        for (double y = std::ceil(box.min.y / spacing) * spacing; y <= box.max.y;
             y += spacing) {
            if (++lineCount > kMaxLines)
                break;
            std::vector<double> xs;
            for (const auto& [p, q] : segs) {
                if ((p.y <= y && q.y > y) || (q.y <= y && p.y > y)) {
                    const double t = (y - p.y) / (q.y - p.y);
                    xs.push_back(p.x + t * (q.x - p.x));
                }
            }
            std::sort(xs.begin(), xs.end());
            for (size_t i = 0; i + 1 < xs.size(); i += 2) {
                StrokePrimitive s;
                s.rgb = ctx.resolvedColor;
                s.points = {Vec2d{xs[i], y}.rotated(theta),
                            Vec2d{xs[i + 1], y}.rotated(theta)};
                out.strokes.push_back(std::move(s));
            }
        }
    }
}

void HatchEntity::geomToJson(QJsonObject& obj) const
{
    QJsonArray loops;
    for (const auto& ring : rings) {
        QJsonArray loop;
        for (const Vec2d& p : ring)
            loop.append(pointToJson(p));
        loops.append(loop);
    }
    obj[QStringLiteral("rings")] = loops;
    obj[QStringLiteral("pattern")] = pattern;
    obj[QStringLiteral("scale")] = scale;
    obj[QStringLiteral("angle")] = angle;
}

void HatchEntity::geomFromJson(const QJsonObject& obj)
{
    rings.clear();
    for (const QJsonValue& loop : obj[QStringLiteral("rings")].toArray()) {
        std::vector<Vec2d> ring;
        for (const QJsonValue& p : loop.toArray())
            ring.push_back(pointFromJson(p));
        if (ring.size() >= 3)
            rings.push_back(std::move(ring));
    }
    pattern = obj[QStringLiteral("pattern")].toString(QStringLiteral("ANSI31"));
    scale = obj[QStringLiteral("scale")].toDouble(1.0);
    angle = obj[QStringLiteral("angle")].toDouble(0.0);
}

} // namespace viki
