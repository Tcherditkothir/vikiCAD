#include "CommandProcessor.h"

#include <QJsonDocument>

#include "doc/Entities.h"
#include "doc/EntitiesEx.h"
#include "geom/GeomUtil.h"
#include "render/HitTest.h"

// Measurement / inquiry commands: DIST, ID, AREA, LIST.
// Results go through ctx.info() — visible in the GUI history and in the
// CLI "messages" array.

namespace viki {
namespace {

QString fmtLen(CommandContext& ctx, double mm, int decimals = 3)
{
    const bool inches = ctx.doc().displayUnits() == DisplayUnits::Inches;
    return QStringLiteral("%1 %2")
        .arg(inches ? mm / 25.4 : mm, 0, 'f', decimals)
        .arg(inches ? QStringLiteral("in") : QStringLiteral("mm"));
}

QString fmtArea(CommandContext& ctx, double mm2)
{
    const bool inches = ctx.doc().displayUnits() == DisplayUnits::Inches;
    return QStringLiteral("%1 %2")
        .arg(inches ? mm2 / (25.4 * 25.4) : mm2, 0, 'f', 3)
        .arg(inches ? QStringLiteral("in²") : QStringLiteral("mm²"));
}

// DIST p1 p2 — distance, deltas and angle.
class DistCommand : public Command {
public:
    const char* name() const override { return "DIST"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point, QStringLiteral("First point:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind != InputValue::Kind::Point)
            return Step::cancelled();
        if (!m_hasFirst) {
            m_p1 = v.point;
            m_hasFirst = true;
            ctx.setLastPoint(v.point);
            return Step::cont(InputKind::Point, QStringLiteral("Second point:"));
        }
        const Vec2d d = v.point - m_p1;
        ctx.info(QStringLiteral("distance = %1   dx = %2   dy = %3   angle = %4°")
                     .arg(fmtLen(ctx, d.length()), fmtLen(ctx, d.x), fmtLen(ctx, d.y))
                     .arg(d.angle() * 180.0 / M_PI, 0, 'f', 2));
        return Step::done();
    }

    void previewAt(CommandContext&, const Vec2d& cursor, PrimitiveList& out) override
    {
        if (!m_hasFirst)
            return;
        StrokePrimitive s;
        s.points = {m_p1, cursor};
        out.strokes.push_back(std::move(s));
    }

private:
    Vec2d m_p1;
    bool m_hasFirst = false;
};

// ID point — coordinates readout.
class IdCommand : public Command {
public:
    const char* name() const override { return "ID"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point, QStringLiteral("Specify point:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind != InputValue::Kind::Point)
            return Step::cancelled();
        ctx.info(QStringLiteral("X = %1   Y = %2")
                     .arg(fmtLen(ctx, v.point.x), fmtLen(ctx, v.point.y)));
        return Step::done();
    }
};

// AREA: click a polygon (Enter to close) or E to pick an entity.
class AreaCommand : public Command {
public:
    const char* name() const override { return "AREA"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point,
                          QStringLiteral("First corner or [E entity]:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        if (m_entityMode) {
            if (v.kind != InputValue::Kind::Point)
                return Step::cancelled();
            return measureEntity(ctx, v.point);
        }
        if (v.kind == InputValue::Kind::Keyword && v.text == QLatin1String("E") &&
            m_points.empty()) {
            m_entityMode = true;
            return Step::cont(InputKind::Point, QStringLiteral("Pick the entity:"));
        }
        if (v.kind == InputValue::Kind::Point) {
            m_points.push_back(v.point);
            ctx.setLastPoint(v.point);
            return Step::cont(InputKind::Point,
                              QStringLiteral("Next corner (Enter to close):"));
        }
        if (v.kind == InputValue::Kind::Finish) {
            if (m_points.size() < 3)
                return Step::cancelled();
            double area2 = 0, perimeter = 0;
            for (size_t i = 0; i < m_points.size(); ++i) {
                const Vec2d& a = m_points[i];
                const Vec2d& b = m_points[(i + 1) % m_points.size()];
                area2 += a.cross(b);
                perimeter += a.distanceTo(b);
            }
            ctx.info(QStringLiteral("area = %1   perimeter = %2")
                         .arg(fmtArea(ctx, std::fabs(area2) / 2.0),
                              fmtLen(ctx, perimeter)));
            return Step::done();
        }
        return Step::cancelled();
    }

    void previewAt(CommandContext&, const Vec2d& cursor, PrimitiveList& out) override
    {
        if (m_points.empty())
            return;
        StrokePrimitive s;
        s.points = m_points;
        s.points.push_back(cursor);
        s.closed = m_points.size() >= 2;
        out.strokes.push_back(std::move(s));
    }

private:
    Step measureEntity(CommandContext& ctx, const Vec2d& pick)
    {
        const EntityId id = hittest::pick(ctx.doc(), pick, ctx.pickTolerance());
        const Entity* e = ctx.doc().entity(id);
        if (!e) {
            ctx.info(QStringLiteral("nothing there"));
            return Step::cont(InputKind::Point, QStringLiteral("Pick the entity:"));
        }
        if (const auto* c = dynamic_cast<const CircleEntity*>(e)) {
            ctx.info(QStringLiteral("circle: area = %1   circumference = %2")
                         .arg(fmtArea(ctx, M_PI * c->radius() * c->radius()),
                              fmtLen(ctx, 2 * M_PI * c->radius())));
            return Step::done();
        }
        if (const auto* pl = dynamic_cast<const PolylineEntity*>(e)) {
            // Flattened shoelace (handles bulges via fine flattening).
            RenderContext rc;
            rc.chordTolerance = 0.01;
            PrimitiveList list;
            pl->buildPrimitives(rc, list);
            if (!list.strokes.empty()) {
                const auto& pts = list.strokes[0].points;
                double area2 = 0, perimeter = 0;
                for (size_t i = 0; i < pts.size(); ++i) {
                    const Vec2d& a = pts[i];
                    const Vec2d& b = pts[(i + 1) % pts.size()];
                    area2 += a.cross(b);
                    perimeter += a.distanceTo(b);
                }
                if (!pl->isClosed())
                    perimeter -= pts.back().distanceTo(pts.front());
                ctx.info(QStringLiteral("polyline: %1area = %2   length = %3")
                             .arg(pl->isClosed() ? QString()
                                                 : QStringLiteral("(open) "),
                                  fmtArea(ctx, std::fabs(area2) / 2.0),
                                  fmtLen(ctx, perimeter)));
                return Step::done();
            }
        }
        ctx.info(QStringLiteral("AREA supports circles and polylines (or click corners)"));
        return Step::done();
    }

    std::vector<Vec2d> m_points;
    bool m_entityMode = false;
};

// LIST: pick an entity, dump a readable summary.
class ListCommand : public Command {
public:
    const char* name() const override { return "LIST"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point, QStringLiteral("Pick an entity:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind != InputValue::Kind::Point)
            return Step::cancelled();
        const EntityId id = hittest::pick(ctx.doc(), v.point, ctx.pickTolerance());
        const Entity* e = ctx.doc().entity(id);
        if (!e) {
            ctx.info(QStringLiteral("nothing there"));
            return Step::cont(InputKind::Point, QStringLiteral("Pick an entity:"));
        }
        const Layer* layer = ctx.doc().layer(e->layerId());
        const BBox2d b = ctx.doc().entityBounds(*e);
        ctx.info(QStringLiteral("#%1  %2  layer '%3'  bounds %4 x %5")
                     .arg(id)
                     .arg(QLatin1String(e->typeName()),
                          layer ? layer->name : QStringLiteral("?"),
                          fmtLen(ctx, b.width(), 2), fmtLen(ctx, b.height(), 2)));
        ctx.info(QString::fromUtf8(
            QJsonDocument(e->toJson()).toJson(QJsonDocument::Compact).left(220)));
        return Step::done();
    }
};

template <typename T>
std::unique_ptr<Command> make()
{
    return std::make_unique<T>();
}

} // namespace

void registerMeasureCommands(CommandProcessor& p)
{
    p.registerCommand(&make<DistCommand>, {QStringLiteral("DI")});
    p.registerCommand(&make<IdCommand>);
    p.registerCommand(&make<AreaCommand>, {QStringLiteral("AA")});
    p.registerCommand(&make<ListCommand>, {QStringLiteral("LI")});
}

} // namespace viki
