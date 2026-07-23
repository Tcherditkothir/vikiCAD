#include "CommandProcessor.h"

#include "doc/EntitiesEx.h"
#include "geom/GeomUtil.h"

// Drawing commands added in M2: RECT, PLINE, ELLIPSE, POINT, XLINE, SPLINE.

namespace viki {
namespace {

// RECT: two corners, or AutoCAD-style Dimensions mode. D/DIMENSIONS at
// either corner prompt asks "Length:" then "Height:", then (when no corner
// was given yet) "First corner:". The rectangle sits axes-aligned from the
// first corner toward +L,+H; NEGATIVE values grow to the other side, exactly
// like AutoCAD's RECTANG Dimensions. No stage here is optional (every prompt
// waits for a specific value), so no doneRepush is needed — a foreign
// keyword just re-prompts.
class RectCommand : public Command {
public:
    const char* name() const override { return "RECT"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point,
                          QStringLiteral("First corner or [Dimensions]:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel || v.kind == InputValue::Kind::Finish)
            return Step::cancelled();

        switch (m_st) {
        case St::First:
            if (isDimensions(v)) {
                m_st = St::Length;
                return Step::cont(InputKind::Distance, QStringLiteral("Length:"));
            }
            if (v.kind != InputValue::Kind::Point)
                return Step::cont(InputKind::Point,
                                  QStringLiteral("First corner or [Dimensions]:"));
            m_first = v.point;
            m_hasFirst = true;
            ctx.setLastPoint(v.point);
            m_st = St::Opposite;
            return oppositePrompt();
        case St::Opposite:
            if (isDimensions(v)) {
                m_st = St::Length;
                return Step::cont(InputKind::Distance, QStringLiteral("Length:"));
            }
            if (v.kind != InputValue::Kind::Point)
                return oppositePrompt();
            return commit(ctx, m_first, v.point);
        case St::Length: {
            const auto len = signedLength(ctx, v);
            if (!len || std::fabs(*len) <= kGeomTol) {
                if (len)
                    ctx.info(QStringLiteral("length must be non-zero"));
                return Step::cont(InputKind::Distance, QStringLiteral("Length:"));
            }
            m_length = *len;
            m_st = St::Height;
            return Step::cont(InputKind::Distance, QStringLiteral("Height:"));
        }
        case St::Height: {
            const auto h = signedLength(ctx, v);
            if (!h || std::fabs(*h) <= kGeomTol) {
                if (h)
                    ctx.info(QStringLiteral("height must be non-zero"));
                return Step::cont(InputKind::Distance, QStringLiteral("Height:"));
            }
            m_height = *h;
            if (m_hasFirst)
                return commit(ctx, m_first,
                              m_first + Vec2d{m_length, m_height});
            m_st = St::Origin;
            return Step::cont(InputKind::Point, QStringLiteral("First corner:"));
        }
        case St::Origin:
            if (v.kind != InputValue::Kind::Point)
                return Step::cont(InputKind::Point, QStringLiteral("First corner:"));
            ctx.setLastPoint(v.point);
            return commit(ctx, v.point, v.point + Vec2d{m_length, m_height});
        }
        return Step::cancelled();
    }

    void previewAt(CommandContext&, const Vec2d& cursor, PrimitiveList& out) override
    {
        Vec2d a, b;
        switch (m_st) {
        case St::First:
            return;
        case St::Opposite:
        case St::Length: // rubber rect while typing/clicking the length
            if (!m_hasFirst)
                return;
            a = m_first;
            b = cursor;
            break;
        case St::Height: // length fixed, height follows the cursor
            if (!m_hasFirst)
                return;
            a = m_first;
            b = {m_first.x + m_length, cursor.y};
            break;
        case St::Origin: // full-size rectangle riding on the cursor
            a = cursor;
            b = cursor + Vec2d{m_length, m_height};
            break;
        }
        StrokePrimitive s;
        s.closed = true;
        s.points = {a, {b.x, a.y}, b, {a.x, b.y}};
        out.strokes.push_back(std::move(s));
    }

private:
    enum class St { First, Opposite, Length, Height, Origin };

    static bool isDimensions(const InputValue& v)
    {
        return v.kind == InputValue::Kind::Keyword &&
               (v.text == QLatin1String("D") ||
                v.text == QLatin1String("DIMENSIONS"));
    }

    static Step oppositePrompt()
    {
        return Step::cont(
            InputKind::Point,
            QStringLiteral("Opposite corner (@dx,dy relative) or [Dimensions]:"));
    }

    // A typed number keeps its SIGN (that is what puts the rectangle on the
    // other side); a clicked point is direct-distance entry from the first
    // corner (or the last point when no corner is placed yet).
    std::optional<double> signedLength(CommandContext& ctx, const InputValue& v) const
    {
        if (v.kind == InputValue::Kind::Number)
            return v.number;
        if (v.kind == InputValue::Kind::Point)
            return v.point.distanceTo(m_hasFirst ? m_first : ctx.lastPoint());
        return std::nullopt;
    }

    Step commit(CommandContext& ctx, const Vec2d& a, const Vec2d& b)
    {
        if (nearEqual(a, b)) {
            ctx.info(QStringLiteral("degenerate rectangle"));
            return Step::cancelled();
        }
        std::vector<PolyVertex> verts{{{a.x, a.y}, 0}, {{b.x, a.y}, 0},
                                      {{b.x, b.y}, 0}, {{a.x, b.y}, 0}};
        ctx.doc().beginTransaction(QStringLiteral("RECT"));
        ctx.doc().addEntity(std::make_unique<PolylineEntity>(std::move(verts), true));
        ctx.doc().commitTransaction();
        ctx.setLastPoint(b);
        return Step::done();
    }

    St m_st = St::First;
    Vec2d m_first;
    bool m_hasFirst = false;
    double m_length = 0.0;
    double m_height = 0.0;
};

// Straight-segment polyline (arc segments arrive with a later milestone).
class PlineCommand : public Command {
public:
    const char* name() const override { return "PLINE"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point, QStringLiteral("Specify start point:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        switch (v.kind) {
        case InputValue::Kind::Point:
            m_verts.push_back({v.point, 0.0});
            ctx.setLastPoint(v.point);
            return Step::cont(InputKind::Point,
                              QStringLiteral("Specify next point (C to close):"));
        case InputValue::Kind::Keyword:
        case InputValue::Kind::Text:
            if (v.text.compare(QLatin1String("C"), Qt::CaseInsensitive) == 0 &&
                m_verts.size() >= 3)
                return commit(ctx, true);
            return Step::cont(InputKind::Point, QStringLiteral("Specify next point:"));
        case InputValue::Kind::Finish:
            if (m_verts.size() < 2)
                return Step::cancelled();
            return commit(ctx, false);
        default:
            return Step::cancelled();
        }
    }

    void previewAt(CommandContext&, const Vec2d& cursor, PrimitiveList& out) override
    {
        if (m_verts.empty())
            return;
        StrokePrimitive s;
        for (const PolyVertex& v : m_verts)
            s.points.push_back(v.pos);
        s.points.push_back(cursor);
        out.strokes.push_back(std::move(s));
    }

private:
    Step commit(CommandContext& ctx, bool closed)
    {
        ctx.doc().beginTransaction(QStringLiteral("PLINE"));
        ctx.doc().addEntity(std::make_unique<PolylineEntity>(std::move(m_verts), closed));
        ctx.doc().commitTransaction();
        return Step::done();
    }
    std::vector<PolyVertex> m_verts;
};

// ELLIPSE: center, major-axis endpoint, then ratio (number) or a point whose
// distance to center over the major length gives the ratio.
class EllipseCommand : public Command {
public:
    const char* name() const override { return "ELLIPSE"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point, QStringLiteral("Specify center:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel || v.kind == InputValue::Kind::Finish)
            return Step::cancelled();

        switch (m_stage) {
        case 0:
            if (v.kind != InputValue::Kind::Point)
                break;
            m_center = v.point;
            ctx.setLastPoint(v.point);
            m_stage = 1;
            return Step::cont(InputKind::Point,
                              QStringLiteral("Specify major axis endpoint:"));
        case 1:
            if (v.kind != InputValue::Kind::Point)
                break;
            m_major = v.point - m_center;
            if (m_major.length() < kGeomTol)
                return Step::cont(InputKind::Point,
                                  QStringLiteral("Specify major axis endpoint:"));
            ctx.setLastPoint(v.point);
            m_stage = 2;
            return Step::cont(InputKind::Distance,
                              QStringLiteral("Specify minor axis length or ratio:"));
        case 2: {
            double minorLen = -1;
            if (v.kind == InputValue::Kind::Number)
                minorLen = v.number <= 1.0 + kGeomTol && v.number > 0
                               ? v.number * m_major.length() // ratio form
                               : v.number;
            else if (v.kind == InputValue::Kind::Point)
                minorLen = v.point.distanceTo(m_center);
            const double major = m_major.length();
            if (minorLen <= kGeomTol || minorLen > major + kGeomTol) {
                ctx.info(QStringLiteral("minor axis must be in (0, major]"));
                return Step::cont(InputKind::Distance,
                                  QStringLiteral("Specify minor axis length or ratio:"));
            }
            ctx.doc().beginTransaction(QStringLiteral("ELLIPSE"));
            ctx.doc().addEntity(
                std::make_unique<EllipseEntity>(m_center, m_major, minorLen / major));
            ctx.doc().commitTransaction();
            return Step::done();
        }
        default:
            break;
        }
        return Step::cont(InputKind::Point, QStringLiteral("Specify point:"));
    }

    void previewAt(CommandContext&, const Vec2d& cursor, PrimitiveList& out) override
    {
        if (m_stage == 1) {
            StrokePrimitive s; // rubber major axis
            s.points = {m_center * 2.0 - cursor, cursor};
            out.strokes.push_back(std::move(s));
        } else if (m_stage == 2) {
            const double major = m_major.length();
            const double minorLen =
                std::clamp(cursor.distanceTo(m_center), kGeomTol, major);
            RenderContext rc;
            rc.chordTolerance = 0.5;
            EllipseEntity(m_center, m_major, minorLen / major).buildPrimitives(rc, out);
        }
    }

private:
    int m_stage = 0;
    Vec2d m_center, m_major;
};

class PointCommand : public Command {
public:
    const char* name() const override { return "POINT"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point, QStringLiteral("Specify point:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Point) {
            if (!ctx.doc().inTransaction())
                ctx.doc().beginTransaction(QStringLiteral("POINT"));
            ctx.doc().addEntity(std::make_unique<PointEntity>(v.point));
            ++m_count;
            ctx.setLastPoint(v.point);
            return Step::cont(InputKind::Point, QStringLiteral("Specify point:"));
        }
        if (v.kind == InputValue::Kind::Finish && m_count > 0)
            return Step::done();
        return Step::cancelled();
    }

private:
    int m_count = 0;
};

// XLINE: base point, then any number of direction points (each creates one
// construction line through the base).
class XLineCommand : public Command {
public:
    const char* name() const override { return "XLINE"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point, QStringLiteral("Specify base point:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        switch (v.kind) {
        case InputValue::Kind::Point:
            if (!m_hasBase) {
                m_base = v.point;
                m_hasBase = true;
                ctx.setLastPoint(v.point);
                return Step::cont(InputKind::Point, QStringLiteral("Specify through point:"));
            }
            if (!nearEqual(v.point, m_base)) {
                if (!ctx.doc().inTransaction())
                    ctx.doc().beginTransaction(QStringLiteral("XLINE"));
                ctx.doc().addEntity(
                    std::make_unique<XLineEntity>(m_base, v.point - m_base));
                ++m_count;
            }
            return Step::cont(InputKind::Point, QStringLiteral("Specify through point:"));
        case InputValue::Kind::Finish:
            return m_count > 0 ? Step::done() : Step::cancelled();
        default:
            return Step::cancelled();
        }
    }

    void previewAt(CommandContext&, const Vec2d& cursor, PrimitiveList& out) override
    {
        if (!m_hasBase || nearEqual(cursor, m_base))
            return;
        const Vec2d dir = (cursor - m_base).normalized();
        StrokePrimitive s;
        s.points = {m_base - dir * 1e5, m_base + dir * 1e5};
        out.strokes.push_back(std::move(s));
    }

private:
    Vec2d m_base;
    bool m_hasBase = false;
    int m_count = 0;
};

// SPLINE: fit points until Enter. Stored as fit-point spline (chord render).
class SplineCommand : public Command {
public:
    const char* name() const override { return "SPLINE"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point, QStringLiteral("Specify first fit point:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        switch (v.kind) {
        case InputValue::Kind::Point:
            m_fit.push_back(v.point);
            ctx.setLastPoint(v.point);
            return Step::cont(InputKind::Point, QStringLiteral("Specify next fit point:"));
        case InputValue::Kind::Finish: {
            if (m_fit.size() < 2)
                return Step::cancelled();
            auto spline = std::make_unique<SplineEntity>();
            spline->fitPoints = std::move(m_fit);
            ctx.doc().beginTransaction(QStringLiteral("SPLINE"));
            ctx.doc().addEntity(std::move(spline));
            ctx.doc().commitTransaction();
            return Step::done();
        }
        default:
            return Step::cancelled();
        }
    }

    void previewAt(CommandContext&, const Vec2d& cursor, PrimitiveList& out) override
    {
        if (m_fit.empty())
            return;
        SplineEntity s;
        s.fitPoints = m_fit;
        s.fitPoints.push_back(cursor);
        RenderContext rc;
        rc.chordTolerance = 0.5;
        s.buildPrimitives(rc, out);
    }

private:
    std::vector<Vec2d> m_fit;
};

template <typename T>
std::unique_ptr<Command> make()
{
    return std::make_unique<T>();
}

} // namespace

void registerDrawCommands2(CommandProcessor& p)
{
    p.registerCommand(&make<RectCommand>,
                      {QStringLiteral("RECTANGLE"), QStringLiteral("REC")});
    p.registerCommand(&make<PlineCommand>, {QStringLiteral("PL")});
    p.registerCommand(&make<EllipseCommand>, {QStringLiteral("EL")});
    p.registerCommand(&make<PointCommand>, {QStringLiteral("PO")});
    p.registerCommand(&make<XLineCommand>, {QStringLiteral("XL")});
    p.registerCommand(&make<SplineCommand>, {QStringLiteral("SPL")});
}

} // namespace viki
