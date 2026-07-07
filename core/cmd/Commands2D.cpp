#include "CommandProcessor.h"

#include "doc/Entities.h"
#include "geom/GeomUtil.h"

namespace viki {
namespace {

// ---- LINE ------------------------------------------------------------------

class LineCommand : public Command {
public:
    const char* name() const override { return "LINE"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point, QStringLiteral("Specify first point:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        switch (v.kind) {
        case InputValue::Kind::Point:
            if (!m_hasPrev) {
                m_prev = v.point;
                m_hasPrev = true;
            } else {
                if (!ctx.doc().inTransaction())
                    ctx.doc().beginTransaction(QStringLiteral("LINE"));
                ctx.doc().addEntity(std::make_unique<LineEntity>(m_prev, v.point));
                ++m_segments;
                m_prev = v.point;
            }
            ctx.setLastPoint(v.point);
            return Step::cont(InputKind::Point, QStringLiteral("Specify next point:"));
        case InputValue::Kind::Finish:
            if (m_segments == 0)
                return Step::cancelled();
            ctx.info(QStringLiteral("%1 line segment(s) drawn").arg(m_segments));
            return Step::done();
        case InputValue::Kind::Cancel:
            return Step::cancelled();
        default:
            return Step::cont(InputKind::Point, QStringLiteral("Specify next point:"));
        }
    }

    void previewAt(CommandContext&, const Vec2d& cursor, PrimitiveList& out) override
    {
        if (!m_hasPrev)
            return;
        StrokePrimitive s;
        s.points = {m_prev, cursor};
        out.strokes.push_back(std::move(s));
    }

private:
    Vec2d m_prev;
    bool m_hasPrev = false;
    int m_segments = 0;
};

// ---- CIRCLE ------------------------------------------------------------------

class CircleCommand : public Command {
public:
    const char* name() const override { return "CIRCLE"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point, QStringLiteral("Specify center point:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel || v.kind == InputValue::Kind::Finish)
            return Step::cancelled();

        if (!m_hasCenter) {
            if (v.kind != InputValue::Kind::Point)
                return Step::cont(InputKind::Point, QStringLiteral("Specify center point:"));
            m_center = v.point;
            m_hasCenter = true;
            ctx.setLastPoint(v.point);
            return Step::cont(InputKind::Distance, QStringLiteral("Specify radius:"));
        }

        double radius = 0;
        if (v.kind == InputValue::Kind::Number)
            radius = v.number;
        else if (v.kind == InputValue::Kind::Point)
            radius = v.point.distanceTo(m_center);
        if (radius <= kGeomTol) {
            ctx.info(QStringLiteral("radius must be positive"));
            return Step::cont(InputKind::Distance, QStringLiteral("Specify radius:"));
        }
        ctx.doc().beginTransaction(QStringLiteral("CIRCLE"));
        ctx.doc().addEntity(std::make_unique<CircleEntity>(m_center, radius));
        ctx.doc().commitTransaction();
        return Step::done();
    }

    void previewAt(CommandContext&, const Vec2d& cursor, PrimitiveList& out) override
    {
        if (!m_hasCenter)
            return;
        StrokePrimitive s;
        s.closed = true;
        flattenArc(m_center, m_center.distanceTo(cursor), 0, 2 * M_PI, 0.0, s.points);
        out.strokes.push_back(std::move(s));
    }

private:
    Vec2d m_center;
    bool m_hasCenter = false;
};

// ---- ARC (3 points) ---------------------------------------------------------

class ArcCommand : public Command {
public:
    const char* name() const override { return "ARC"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point, QStringLiteral("Specify start point of arc:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel || v.kind == InputValue::Kind::Finish)
            return Step::cancelled();
        if (v.kind != InputValue::Kind::Point)
            return Step::cont(InputKind::Point, prompt());

        m_points[m_count++] = v.point;
        ctx.setLastPoint(v.point);
        if (m_count < 3)
            return Step::cont(InputKind::Point, prompt());

        const auto circle = circleFrom3Points(m_points[0], m_points[1], m_points[2]);
        if (!circle) {
            ctx.info(QStringLiteral("points are collinear — no arc created"));
            return Step::cancelled();
        }
        const double a1 = (m_points[0] - circle->center).angle();
        const double a2 = (m_points[1] - circle->center).angle();
        const double a3 = (m_points[2] - circle->center).angle();
        // CCW arc from a1 to a3 if it passes through a2, else the CCW arc
        // from a3 to a1 (same geometry as CW from a1).
        double start = a1, sweep = ccwSweep(a1, a3);
        if (!angleOnArc(a2, a1, sweep)) {
            start = a3;
            sweep = ccwSweep(a3, a1);
        }
        ctx.doc().beginTransaction(QStringLiteral("ARC"));
        ctx.doc().addEntity(
            std::make_unique<ArcEntity>(circle->center, circle->radius, start, sweep));
        ctx.doc().commitTransaction();
        return Step::done();
    }

private:
    QString prompt() const
    {
        switch (m_count) {
        case 0: return QStringLiteral("Specify start point of arc:");
        case 1: return QStringLiteral("Specify second point on arc:");
        default: return QStringLiteral("Specify end point of arc:");
        }
    }
    Vec2d m_points[3];
    int m_count = 0;
};

// ---- ERASE -------------------------------------------------------------------

class EraseCommand : public Command {
public:
    const char* name() const override { return "ERASE"; }

    Step start(CommandContext& ctx) override
    {
        // Pickfirst: a pre-existing selection is consumed immediately.
        if (!ctx.selection().isEmpty()) {
            eraseIds(ctx, ctx.selection().ids());
            ctx.selection().clear();
            return Step::done();
        }
        return Step::cont(InputKind::EntitySet, QStringLiteral("Select objects:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        switch (v.kind) {
        case InputValue::Kind::EntitySet:
            eraseIds(ctx, v.entitySet);
            return Step::done();
        case InputValue::Kind::EntityRef:
            m_picked.push_back(v.entityRef);
            return Step::cont(InputKind::EntitySet, QStringLiteral("Select objects:"));
        case InputValue::Kind::Finish:
            if (m_picked.empty())
                return Step::cancelled();
            eraseIds(ctx, m_picked);
            return Step::done();
        default:
            return Step::cancelled();
        }
    }

private:
    static void eraseIds(CommandContext& ctx, const std::vector<EntityId>& ids)
    {
        ctx.doc().beginTransaction(QStringLiteral("ERASE"));
        int n = 0;
        for (const EntityId id : ids)
            if (ctx.doc().removeEntity(id))
                ++n;
        ctx.doc().commitTransaction();
        ctx.info(QStringLiteral("%1 erased").arg(n));
    }
    std::vector<EntityId> m_picked;
};

// ---- UNDO / REDO ---------------------------------------------------------------

class UndoCommand : public Command {
public:
    const char* name() const override { return "UNDO"; }
    Step start(CommandContext& ctx) override
    {
        const QString name = ctx.doc().undo();
        ctx.info(name.isEmpty() ? QStringLiteral("nothing to undo")
                                : QStringLiteral("undid %1").arg(name));
        return Step::done();
    }
    Step onInput(CommandContext&, const InputValue&) override { return Step::done(); }
};

class RedoCommand : public Command {
public:
    const char* name() const override { return "REDO"; }
    Step start(CommandContext& ctx) override
    {
        const QString name = ctx.doc().redo();
        ctx.info(name.isEmpty() ? QStringLiteral("nothing to redo")
                                : QStringLiteral("redid %1").arg(name));
        return Step::done();
    }
    Step onInput(CommandContext&, const InputValue&) override { return Step::done(); }
};

// ---- ZOOM ----------------------------------------------------------------------

class ZoomCommand : public Command {
public:
    const char* name() const override { return "ZOOM"; }
    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Keyword, QStringLiteral("Enter option [Extents]:"));
    }
    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Keyword &&
            (v.text == QLatin1String("E") || v.text == QLatin1String("EXTENTS"))) {
            if (ctx.view())
                ctx.view()->zoomExtents();
            return Step::done();
        }
        if (v.kind == InputValue::Kind::Cancel || v.kind == InputValue::Kind::Finish)
            return Step::cancelled();
        ctx.info(QStringLiteral("unknown zoom option"));
        return Step::cont(InputKind::Keyword, QStringLiteral("Enter option [Extents]:"));
    }
};

template <typename T>
std::unique_ptr<Command> make()
{
    return std::make_unique<T>();
}

} // namespace

void registerBuiltinCommands(CommandProcessor& p)
{
    p.registerCommand(&make<LineCommand>, {QStringLiteral("L")});
    p.registerCommand(&make<CircleCommand>, {QStringLiteral("C")});
    p.registerCommand(&make<ArcCommand>, {QStringLiteral("A")});
    p.registerCommand(&make<EraseCommand>, {QStringLiteral("E"), QStringLiteral("DEL")});
    p.registerCommand(&make<UndoCommand>, {QStringLiteral("U")});
    p.registerCommand(&make<RedoCommand>);
    p.registerCommand(&make<ZoomCommand>, {QStringLiteral("Z")});
    registerDrawCommands2(p);
    registerModifyCommands(p);
    registerEditCommands(p);
    registerAnnotateCommands(p);
    registerBlockCommands(p);
    registerArrayNoteCommands(p);
    registerLayoutCommands(p);
    registerSolidCommands(p);
}

} // namespace viki
