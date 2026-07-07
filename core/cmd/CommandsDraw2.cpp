#include "CommandProcessor.h"

#include "doc/EntitiesEx.h"
#include "geom/GeomUtil.h"

// Drawing commands added in M2: RECT, PLINE, ELLIPSE, POINT, XLINE, SPLINE.

namespace viki {
namespace {

class RectCommand : public Command {
public:
    const char* name() const override { return "RECT"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point, QStringLiteral("Specify first corner:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel || v.kind == InputValue::Kind::Finish)
            return Step::cancelled();
        if (v.kind != InputValue::Kind::Point)
            return Step::cont(InputKind::Point, QStringLiteral("Specify corner:"));

        if (!m_hasFirst) {
            m_first = v.point;
            m_hasFirst = true;
            ctx.setLastPoint(v.point);
            return Step::cont(InputKind::Point, QStringLiteral("Specify opposite corner:"));
        }
        const Vec2d a = m_first;
        const Vec2d b = v.point;
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

    void previewAt(CommandContext&, const Vec2d& cursor, PrimitiveList& out) override
    {
        if (!m_hasFirst)
            return;
        StrokePrimitive s;
        s.closed = true;
        s.points = {m_first, {cursor.x, m_first.y}, cursor, {m_first.x, cursor.y}};
        out.strokes.push_back(std::move(s));
    }

private:
    Vec2d m_first;
    bool m_hasFirst = false;
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
    p.registerCommand(&make<RectCommand>, {QStringLiteral("RECTANGLE")});
    p.registerCommand(&make<PlineCommand>, {QStringLiteral("PL")});
    p.registerCommand(&make<EllipseCommand>, {QStringLiteral("EL")});
    p.registerCommand(&make<PointCommand>, {QStringLiteral("PO")});
    p.registerCommand(&make<XLineCommand>, {QStringLiteral("XL")});
    p.registerCommand(&make<SplineCommand>, {QStringLiteral("SPL")});
}

} // namespace viki
