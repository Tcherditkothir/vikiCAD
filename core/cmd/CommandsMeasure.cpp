#include "CommandProcessor.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <gp_Pln.hxx>

#include "doc/Entities.h"
#include "doc/EntitiesEx.h"
#include "edit/MinDist.h"
#include "geom/GeomUtil.h"
#include "render/HitTest.h"
#include "solid/SolidEntity.h"
#include "solid/SolidMetrics.h"
#include "solid/SolidOps.h"

// Measurement / inquiry commands: DIST, ID, AREA, LIST, MINDIST.
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
        // Solids get the quick numbers too — the same metrics DESCRIBE
        // prints (shared solidops::solidMetrics helper), same formatting.
        if (const auto* solid = dynamic_cast<const SolidEntity*>(e)) {
            const auto m = solidops::solidMetrics(solid->shape());
            const auto num = [](double x) {
                // Fixed one decimal; normalise the OCCT bbox fringe "-0.0".
                const QString s = QString::number(x, 'f', 1);
                return s == QLatin1String("-0.0") ? QStringLiteral("0.0") : s;
            };
            const auto triple = [&num](const gp_Pnt& p) {
                return QStringLiteral("(%1,%2,%3)")
                    .arg(num(p.X()), num(p.Y()), num(p.Z()));
            };
            ctx.info(QStringLiteral(
                         "volume=%1 mm3 area=%2 mm2 bbox=%3-%4 features=%5")
                         .arg(num(m.volume), num(m.area), triple(m.bboxMin),
                              triple(m.bboxMax))
                         .arg(solid->features ? solid->features->count() : 0));
        }
        ctx.info(QString::fromUtf8(
            QJsonDocument(e->toJson()).toJson(QJsonDocument::Compact).left(220)));
        return Step::done();
    }
};

// MINDIST [idA idB] — the CAM clearance measurement: minimum EDGE-TO-EDGE
// distance between two entities with material semantics (wide trace = its
// round-capped footprint, circle/drill = a disk of its radius, flashed pad =
// the real aperture footprint of its GBR-* block). Honors a pre-existing
// two-entity selection. Output: a human line, the closest-points line, and
// one machine-readable JSON line ({"mindist":{...}}, mm). A witness line
// stays on the canvas until the next command starts.
class MinDistCommand : public Command {
public:
    const char* name() const override { return "MINDIST"; }

    Step start(CommandContext& ctx) override
    {
        const auto sel = ctx.selection().ids();
        if (sel.size() >= 2)
            return report(ctx, sel[0], sel[1]);
        return Step::cont(InputKind::EntitySet,
                          QStringLiteral("Select two entities:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        switch (v.kind) {
        case InputValue::Kind::EntitySet:
            for (const EntityId id : v.entitySet)
                m_ids.push_back(id);
            break;
        case InputValue::Kind::EntityRef:
            m_ids.push_back(v.entityRef);
            break;
        case InputValue::Kind::Finish:
            if (m_ids.size() >= 2)
                break;
            ctx.info(QStringLiteral("MINDIST needs two entities"));
            return Step::done();
        default:
            return Step::cancelled();
        }
        if (m_ids.size() < 2)
            return Step::cont(InputKind::EntitySet,
                              QStringLiteral("Select the second entity:"));
        return report(ctx, m_ids[0], m_ids[1]);
    }

private:
    Step report(CommandContext& ctx, EntityId a, EntityId b)
    {
        const auto r = measure::minDistance(ctx.doc(), a, b);
        if (!r.ok) {
            ctx.info(r.error);
            return Step::done();
        }
        const QString method = r.exact ? QStringLiteral("exact")
                                       : QStringLiteral("bbox");
        for (const QString& note : r.notes)
            ctx.info(note);
        if (r.overlap)
            ctx.info(QStringLiteral("#%1 and #%2 touch or overlap — "
                                    "edge-to-edge distance = 0 (%3)")
                         .arg(a)
                         .arg(b)
                         .arg(method));
        else
            ctx.info(QStringLiteral("min edge-to-edge distance #%1 -> #%2 = %3 (%4)")
                         .arg(a)
                         .arg(b)
                         .arg(fmtLen(ctx, r.distance, 4), method));
        ctx.info(QStringLiteral("closest points: (%1, %2) -> (%3, %4) mm")
                     .arg(r.pa.x, 0, 'f', 4)
                     .arg(r.pa.y, 0, 'f', 4)
                     .arg(r.pb.x, 0, 'f', 4)
                     .arg(r.pb.y, 0, 'f', 4));
        // Machine-readable trailer (mm, full precision) for agents/scripts.
        const QJsonObject json{
            {QStringLiteral("mindist"),
             QJsonObject{{QStringLiteral("a"), qint64(a)},
                         {QStringLiteral("b"), qint64(b)},
                         {QStringLiteral("mm"), r.distance},
                         {QStringLiteral("overlap"), r.overlap},
                         {QStringLiteral("method"), method},
                         {QStringLiteral("pa"), QJsonArray{r.pa.x, r.pa.y}},
                         {QStringLiteral("pb"), QJsonArray{r.pb.x, r.pb.y}}}}};
        ctx.info(QString::fromUtf8(
            QJsonDocument(json).toJson(QJsonDocument::Compact)));

        // Witness overlay: the clearance segment with a tick at each end
        // (an X at the single contact point when overlapping).
        PrimitiveList overlay;
        const auto tick = [&](const Vec2d& at) {
            const double t = 0.4; // mm
            StrokePrimitive s1, s2;
            s1.points = {at + Vec2d{-t, -t}, at + Vec2d{t, t}};
            s2.points = {at + Vec2d{-t, t}, at + Vec2d{t, -t}};
            overlay.strokes.push_back(std::move(s1));
            overlay.strokes.push_back(std::move(s2));
        };
        tick(r.pa);
        if (!r.overlap) {
            tick(r.pb);
            StrokePrimitive line;
            line.points = {r.pa, r.pb};
            overlay.strokes.push_back(std::move(line));
        }
        ctx.setOverlay(std::move(overlay));
        return Step::done();
    }

    std::vector<EntityId> m_ids;
};

// INTERFERE [id id]: assembly clash check. With two solid ids, report the
// overlap (interference) volume of that pair. With no ids, sweep every solid
// pair in the document and report all that interpenetrate. The ids come as one
// EntitySet gulp, so "INTERFERE 1 2" works headless.
class InterfereCommand : public Command {
public:
    const char* name() const override { return "INTERFERE"; }

    Step start(CommandContext& ctx) override
    {
        // Honor a pre-existing selection (e.g. picked in the GUI).
        const auto sel = ctx.selection().ids();
        if (sel.size() >= 2)
            return report(ctx, sel);
        return Step::cont(InputKind::EntitySet,
                          QStringLiteral("Select two solids (Enter to check all):"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        std::vector<EntityId> ids;
        switch (v.kind) {
        case InputValue::Kind::EntitySet:
            ids = v.entitySet;
            break;
        case InputValue::Kind::EntityRef:
            ids.push_back(v.entityRef);
            break;
        case InputValue::Kind::Finish:
            break; // sweep all
        case InputValue::Kind::Cancel:
            return Step::cancelled();
        default:
            return Step::cancelled();
        }
        return report(ctx, ids);
    }

private:
    QString fmtVol(CommandContext& ctx, double mm3) const
    {
        const bool inches = ctx.doc().displayUnits() == DisplayUnits::Inches;
        const double cube = 25.4 * 25.4 * 25.4;
        return QStringLiteral("%1 %2")
            .arg(inches ? mm3 / cube : mm3, 0, 'f', 3)
            .arg(inches ? QStringLiteral("in³") : QStringLiteral("mm³"));
    }

    Step report(CommandContext& ctx, const std::vector<EntityId>& ids)
    {
        if (ids.size() >= 2) {
            const auto* a = dynamic_cast<const SolidEntity*>(ctx.doc().entity(ids[0]));
            const auto* b = dynamic_cast<const SolidEntity*>(ctx.doc().entity(ids[1]));
            if (!a || !b) {
                ctx.info(QStringLiteral("INTERFERE needs two solids"));
                return Step::done();
            }
            const double v = solidops::interferenceVolume(a->shape(), b->shape());
            if (v > 0.0)
                ctx.info(QStringLiteral("#%1 and #%2 interfere: overlap = %3")
                             .arg(ids[0])
                             .arg(ids[1])
                             .arg(fmtVol(ctx, v)));
            else
                ctx.info(QStringLiteral("#%1 and #%2 do not interfere")
                             .arg(ids[0])
                             .arg(ids[1]));
            return Step::done();
        }
        // Sweep the whole document.
        const auto pairs = solidops::checkAllInterferences(ctx.doc());
        if (pairs.empty()) {
            ctx.info(QStringLiteral("no interferences found"));
            return Step::done();
        }
        ctx.info(QStringLiteral("%1 interfering pair(s):").arg(pairs.size()));
        for (const auto& p : pairs)
            ctx.info(QStringLiteral("  #%1 <-> #%2  overlap = %3")
                         .arg(p.a)
                         .arg(p.b)
                         .arg(fmtVol(ctx, p.volume)));
        return Step::done();
    }
};

// SECTION [XY/XZ/YZ] offset id — cut a solid by an axis-aligned plane at the
// given signed offset (mm along the plane normal) and report the cross-section
// area. Plane keyword and numeric offset come BEFORE the entity, so the greedy
// EntitySet does not swallow the offset number.
class SectionCommand : public Command {
public:
    const char* name() const override { return "SECTION"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Keyword,
                          QStringLiteral("Section plane [XY / XZ / YZ] <XY>:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        switch (m_stage) {
        case Stage::Plane: {
            if (v.kind == InputValue::Kind::Keyword) {
                const QString kw = v.text.toUpper();
                if (kw == QLatin1String("XZ"))
                    m_normal = gp_Dir(0, 1, 0);
                else if (kw == QLatin1String("YZ"))
                    m_normal = gp_Dir(1, 0, 0);
                else
                    m_normal = gp_Dir(0, 0, 1); // XY / anything else
            } else if (v.kind == InputValue::Kind::Finish) {
                m_normal = gp_Dir(0, 0, 1); // default XY
            } else {
                return Step::cancelled();
            }
            m_stage = Stage::Offset;
            return Step::cont(InputKind::Distance,
                              QStringLiteral("Offset along normal <0>:"));
        }
        case Stage::Offset: {
            if (v.kind == InputValue::Kind::Number)
                m_offset = v.number;
            else if (v.kind != InputValue::Kind::Finish)
                return Step::cancelled();
            m_stage = Stage::Entity;
            // Honor a pre-existing selection (e.g. GUI pick).
            const auto sel = ctx.selection().ids();
            if (!sel.empty())
                return report(ctx, sel.front());
            return Step::cont(InputKind::EntitySet,
                              QStringLiteral("Select the solid:"));
        }
        case Stage::Entity: {
            EntityId id = 0;
            if (v.kind == InputValue::Kind::EntitySet && !v.entitySet.empty())
                id = v.entitySet.front();
            else if (v.kind == InputValue::Kind::EntityRef)
                id = v.entityRef;
            else
                return Step::cancelled();
            return report(ctx, id);
        }
        }
        return Step::cancelled();
    }

private:
    enum class Stage { Plane, Offset, Entity };

    Step report(CommandContext& ctx, EntityId id)
    {
        const auto* s = dynamic_cast<const SolidEntity*>(ctx.doc().entity(id));
        if (!s) {
            ctx.info(QStringLiteral("SECTION needs a solid"));
            return Step::done();
        }
        // Plane through offset*normal with the chosen normal.
        const gp_Pnt origin(m_normal.X() * m_offset, m_normal.Y() * m_offset,
                            m_normal.Z() * m_offset);
        const gp_Pln pln(origin, m_normal);
        const double area = solidops::sectionArea(s->shape(), pln);
        if (area > 0.0)
            ctx.info(QStringLiteral("#%1 section area = %2")
                         .arg(id)
                         .arg(fmtArea(ctx, area)));
        else
            ctx.info(QStringLiteral("#%1: plane does not cut the solid").arg(id));
        return Step::done();
    }

    gp_Dir m_normal{0, 0, 1};
    double m_offset = 0.0;
    Stage m_stage = Stage::Plane;
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
    p.registerCommand(&make<MinDistCommand>, {QStringLiteral("MD")});
    p.registerCommand(&make<InterfereCommand>, {QStringLiteral("CLASH")});
    p.registerCommand(&make<SectionCommand>, {QStringLiteral("SEC")});
}

} // namespace viki
