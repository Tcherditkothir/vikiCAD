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
// Modes: center+radius (default), D (center+diameter value), 2P (diameter
// endpoints), 3P (three points on the circle).

class CircleCommand : public Command {
public:
    const char* name() const override { return "CIRCLE"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point,
                          QStringLiteral("Specify center point or [2P/3P]:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel || v.kind == InputValue::Kind::Finish)
            return Step::cancelled();

        if (v.kind == InputValue::Kind::Keyword) {
            if (v.text == QLatin1String("2P") && m_points.empty()) {
                m_mode = Mode::TwoPoint;
                return Step::cont(InputKind::Point,
                                  QStringLiteral("First diameter endpoint:"));
            }
            if (v.text == QLatin1String("3P") && m_points.empty()) {
                m_mode = Mode::ThreePoint;
                return Step::cont(InputKind::Point, QStringLiteral("First point:"));
            }
            if (v.text == QLatin1String("D") && m_mode == Mode::Center &&
                m_points.size() == 1) {
                m_diameter = true;
                return Step::cont(InputKind::Distance, QStringLiteral("Specify diameter:"));
            }
            return currentPrompt();
        }

        switch (m_mode) {
        case Mode::Center: {
            if (m_points.empty()) {
                if (v.kind != InputValue::Kind::Point)
                    return currentPrompt();
                m_points.push_back(v.point);
                ctx.setLastPoint(v.point);
                return Step::cont(InputKind::Distance,
                                  QStringLiteral("Specify radius or [D]:"));
            }
            double radius = 0;
            if (v.kind == InputValue::Kind::Number)
                radius = m_diameter ? v.number / 2.0 : v.number;
            else if (v.kind == InputValue::Kind::Point)
                radius = v.point.distanceTo(m_points[0]);
            return commit(ctx, m_points[0], radius);
        }
        case Mode::TwoPoint: {
            if (v.kind != InputValue::Kind::Point)
                return currentPrompt();
            m_points.push_back(v.point);
            ctx.setLastPoint(v.point);
            if (m_points.size() < 2)
                return Step::cont(InputKind::Point,
                                  QStringLiteral("Second diameter endpoint:"));
            return commit(ctx, (m_points[0] + m_points[1]) * 0.5,
                          m_points[0].distanceTo(m_points[1]) / 2.0);
        }
        case Mode::ThreePoint: {
            if (v.kind != InputValue::Kind::Point)
                return currentPrompt();
            m_points.push_back(v.point);
            ctx.setLastPoint(v.point);
            if (m_points.size() < 3)
                return Step::cont(InputKind::Point,
                                  m_points.size() == 1
                                      ? QStringLiteral("Second point:")
                                      : QStringLiteral("Third point:"));
            const auto c = circleFrom3Points(m_points[0], m_points[1], m_points[2]);
            if (!c) {
                ctx.info(QStringLiteral("points are collinear"));
                return Step::cancelled();
            }
            return commit(ctx, c->center, c->radius);
        }
        }
        return Step::cancelled();
    }

    void previewAt(CommandContext&, const Vec2d& cursor, PrimitiveList& out) override
    {
        std::optional<std::pair<Vec2d, double>> circle;
        std::vector<Vec2d> construction; // rubber lines to placed points
        switch (m_mode) {
        case Mode::Center:
            if (!m_points.empty() && !m_diameter) {
                circle = {{m_points[0], m_points[0].distanceTo(cursor)}};
                construction = {m_points[0], cursor};
            }
            break;
        case Mode::TwoPoint:
            if (m_points.size() == 1) {
                circle = {{(m_points[0] + cursor) * 0.5,
                           m_points[0].distanceTo(cursor) / 2.0}};
                construction = {m_points[0], cursor};
            }
            break;
        case Mode::ThreePoint:
            if (m_points.size() == 1) {
                construction = {m_points[0], cursor};
            } else if (m_points.size() == 2) {
                if (const auto c = circleFrom3Points(m_points[0], m_points[1], cursor))
                    circle = {{c->center, c->radius}};
                construction = {m_points[0], m_points[1], m_points[1], cursor};
            }
            break;
        }
        if (!construction.empty()) {
            for (size_t i = 0; i + 1 < construction.size(); i += 2) {
                StrokePrimitive line;
                line.points = {construction[i], construction[i + 1]};
                out.strokes.push_back(std::move(line));
            }
        }
        if (circle && circle->second > kGeomTol) {
            StrokePrimitive s;
            s.closed = true;
            flattenArc(circle->first, circle->second, 0, 2 * M_PI, 0.0, s.points);
            out.strokes.push_back(std::move(s));
        }
    }

private:
    enum class Mode { Center, TwoPoint, ThreePoint };

    Step currentPrompt() const
    {
        switch (m_mode) {
        case Mode::Center:
            return Step::cont(InputKind::Point,
                              m_points.empty()
                                  ? QStringLiteral("Specify center point or [2P/3P]:")
                                  : QStringLiteral("Specify radius or [D]:"));
        case Mode::TwoPoint:
            return Step::cont(InputKind::Point, QStringLiteral("Diameter endpoint:"));
        case Mode::ThreePoint:
            return Step::cont(InputKind::Point, QStringLiteral("Point on circle:"));
        }
        return Step::cancelled();
    }

    Step commit(CommandContext& ctx, const Vec2d& center, double radius)
    {
        if (radius <= kGeomTol) {
            ctx.info(QStringLiteral("radius must be positive"));
            return currentPrompt();
        }
        ctx.doc().beginTransaction(QStringLiteral("CIRCLE"));
        ctx.doc().addEntity(std::make_unique<CircleEntity>(center, radius));
        ctx.doc().commitTransaction();
        return Step::done();
    }

    Mode m_mode = Mode::Center;
    std::vector<Vec2d> m_points;
    bool m_diameter = false;
};

// ---- ARC ---------------------------------------------------------------------
// Modes: 3 points (default) or CE (center, start point, end direction).

class ArcCommand : public Command {
public:
    const char* name() const override { return "ARC"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point,
                          QStringLiteral("Specify start point of arc or [CE]:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel || v.kind == InputValue::Kind::Finish)
            return Step::cancelled();
        if (v.kind == InputValue::Kind::Keyword) {
            if (v.text == QLatin1String("CE") && m_count == 0) {
                m_centerMode = true;
                return Step::cont(InputKind::Point, QStringLiteral("Specify center:"));
            }
            return Step::cont(InputKind::Point, prompt());
        }
        if (v.kind != InputValue::Kind::Point)
            return Step::cont(InputKind::Point, prompt());

        m_points[m_count++] = v.point;
        ctx.setLastPoint(v.point);
        if (m_count < 3)
            return Step::cont(InputKind::Point, prompt());

        if (m_centerMode) {
            // center, start, end-direction: CCW from start toward the end ray.
            const Vec2d center = m_points[0];
            const double radius = center.distanceTo(m_points[1]);
            if (radius <= kGeomTol) {
                ctx.info(QStringLiteral("degenerate arc"));
                return Step::cancelled();
            }
            const double start = (m_points[1] - center).angle();
            const double end = (m_points[2] - center).angle();
            ctx.doc().beginTransaction(QStringLiteral("ARC"));
            ctx.doc().addEntity(std::make_unique<ArcEntity>(center, radius, start,
                                                            ccwSweep(start, end)));
            ctx.doc().commitTransaction();
            return Step::done();
        }

        const auto circle = circleFrom3Points(m_points[0], m_points[1], m_points[2]);
        if (!circle) {
            ctx.info(QStringLiteral("points are collinear — no arc created"));
            return Step::cancelled();
        }
        const double a1 = (m_points[0] - circle->center).angle();
        const double a2 = (m_points[1] - circle->center).angle();
        const double a3 = (m_points[2] - circle->center).angle();
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

    void previewAt(CommandContext&, const Vec2d& cursor, PrimitiveList& out) override
    {
        const auto pushLine = [&](const Vec2d& a, const Vec2d& b) {
            StrokePrimitive s;
            s.points = {a, b};
            out.strokes.push_back(std::move(s));
        };
        const auto pushArc = [&](const Vec2d& c, double r, double a0, double sweep) {
            if (r <= kGeomTol || sweep <= kGeomTol)
                return;
            StrokePrimitive s;
            flattenArc(c, r, a0, sweep, 0.0, s.points);
            out.strokes.push_back(std::move(s));
        };

        if (m_centerMode) {
            if (m_count == 1) { // center placed: radius rubber line
                pushLine(m_points[0], cursor);
            } else if (m_count == 2) { // start placed: live arc to cursor ray
                const Vec2d center = m_points[0];
                const double radius = center.distanceTo(m_points[1]);
                const double start = (m_points[1] - center).angle();
                const double end = (cursor - center).angle();
                pushLine(center, m_points[1]);
                pushLine(center, cursor);
                pushArc(center, radius, start, ccwSweep(start, end));
            }
            return;
        }
        if (m_count == 1) { // rubber chord
            pushLine(m_points[0], cursor);
        } else if (m_count == 2) { // live 3-point arc through the cursor
            pushLine(m_points[0], m_points[1]);
            if (const auto c = circleFrom3Points(m_points[0], m_points[1], cursor)) {
                const double a1 = (m_points[0] - c->center).angle();
                const double a2 = (m_points[1] - c->center).angle();
                const double a3 = (cursor - c->center).angle();
                double start = a1, sweep = ccwSweep(a1, a3);
                if (!angleOnArc(a2, a1, sweep)) {
                    start = a3;
                    sweep = ccwSweep(a3, a1);
                }
                pushArc(c->center, c->radius, start, sweep);
            }
        }
    }

private:
    QString prompt() const
    {
        if (m_centerMode) {
            switch (m_count) {
            case 0: return QStringLiteral("Specify center:");
            case 1: return QStringLiteral("Specify start point:");
            default: return QStringLiteral("Specify end point (CCW):");
            }
        }
        switch (m_count) {
        case 0: return QStringLiteral("Specify start point of arc or [CE]:");
        case 1: return QStringLiteral("Specify second point on arc:");
        default: return QStringLiteral("Specify end point of arc:");
        }
    }
    Vec2d m_points[3];
    int m_count = 0;
    bool m_centerMode = false;
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
    registerSolidFinishCommands(p);
}

} // namespace viki
