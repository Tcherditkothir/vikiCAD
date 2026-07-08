#include "CommandProcessor.h"

#include "doc/Annotations.h"
#include "doc/Entities.h"
#include "doc/EntitiesEx.h"
#include "render/HitTest.h"

// M4 annotation commands: TEXT, DIMLINEAR/DIMALIGNED/DIMANGULAR/DIMRADIUS/
// DIMDIAMETER, LEADER, HATCH, DIMSTYLE.

namespace viki {
namespace {

// TEXT position height rotation_deg text...
class TextCommand : public Command {
public:
    const char* name() const override { return "TEXT"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point, QStringLiteral("Specify insertion point:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        switch (m_stage) {
        case 0:
            if (v.kind != InputValue::Kind::Point)
                return Step::cancelled();
            m_pos = v.point;
            ctx.setLastPoint(v.point);
            m_stage = 1;
            return Step::cont(InputKind::Distance,
                              QStringLiteral("Specify text height <3.5>:"));
        case 1:
            if (v.kind == InputValue::Kind::Number)
                m_height = std::max(0.1, v.number);
            else if (v.kind != InputValue::Kind::Finish)
                return Step::cancelled();
            m_stage = 2;
            return Step::cont(InputKind::Number,
                              QStringLiteral("Specify rotation (degrees) <0>:"));
        case 2:
            if (v.kind == InputValue::Kind::Number)
                m_rotation = v.number * M_PI / 180.0;
            else if (v.kind != InputValue::Kind::Finish)
                return Step::cancelled();
            m_stage = 3;
            return Step::cont(InputKind::Text, QStringLiteral("Enter text:"));
        case 3: {
            if (v.kind != InputValue::Kind::Text || v.text.trimmed().isEmpty())
                return Step::cancelled();
            // Literal \n sequences become real line breaks.
            QString content = v.text;
            content.replace(QLatin1String("\\n"), QLatin1String("\n"));
            ctx.doc().beginTransaction(QStringLiteral("TEXT"));
            ctx.doc().addEntity(
                std::make_unique<TextEntity>(m_pos, m_height, m_rotation, content));
            ctx.doc().commitTransaction();
            return Step::done();
        }
        default:
            return Step::cancelled();
        }
    }

private:
    int m_stage = 0;
    Vec2d m_pos;
    double m_height = 3.5;
    double m_rotation = 0.0;
};

// TEXTEDIT: pick a text entity, replace its content. Text gulps rest of line.
class TextEditCommand : public Command {
public:
    const char* name() const override { return "TEXTEDIT"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::EntitySet, QStringLiteral("Select text to edit:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        if (m_stage == 0) {
            EntityId id = kInvalidEntityId;
            if (v.kind == InputValue::Kind::EntityRef)
                id = v.entityRef;
            else if (v.kind == InputValue::Kind::EntitySet && !v.entitySet.empty())
                id = v.entitySet.front();
            else if (v.kind == InputValue::Kind::Point)
                id = hittest::pick(ctx.doc(), v.point, ctx.pickTolerance());
            const auto* t = dynamic_cast<const TextEntity*>(ctx.doc().entity(id));
            if (!t) {
                ctx.info(QStringLiteral("not a text entity"));
                return Step::cancelled();
            }
            m_id = id;
            m_stage = 1;
            return Step::cont(InputKind::Text,
                              QStringLiteral("Enter new text <%1>:")
                                  .arg(QString(t->text()).replace(QLatin1Char('\n'),
                                                                  QLatin1String("\\n"))));
        }
        if (v.kind != InputValue::Kind::Text || v.text.trimmed().isEmpty())
            return Step::cancelled();
        QString content = v.text;
        content.replace(QLatin1String("\\n"), QLatin1String("\n"));
        ctx.doc().beginTransaction(QStringLiteral("TEXTEDIT"));
        if (auto* e = dynamic_cast<TextEntity*>(ctx.doc().beginModify(m_id))) {
            e->setText(content);
            ctx.doc().endModify(m_id);
        }
        ctx.doc().commitTransaction();
        return Step::done();
    }

private:
    int m_stage = 0;
    EntityId m_id = kInvalidEntityId;
};

// Linear/aligned dimensions: two def points + dimension line position.
class DimLinearCommand : public Command {
public:
    explicit DimLinearCommand(bool aligned) : m_aligned(aligned) {}
    const char* name() const override { return m_aligned ? "DIMALIGNED" : "DIMLINEAR"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point, QStringLiteral("First extension line origin:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind != InputValue::Kind::Point)
            return Step::cancelled();
        switch (m_stage) {
        case 0:
            m_a = v.point;
            m_stage = 1;
            ctx.setLastPoint(v.point);
            return Step::cont(InputKind::Point,
                              QStringLiteral("Second extension line origin:"));
        case 1:
            m_b = v.point;
            m_stage = 2;
            ctx.setLastPoint(v.point);
            return Step::cont(InputKind::Point,
                              QStringLiteral("Dimension line location:"));
        case 2: {
            auto dim = std::make_unique<DimensionEntity>();
            dim->kind = m_aligned ? DimensionEntity::Kind::Aligned
                                  : DimensionEntity::Kind::Linear;
            dim->a = m_a;
            dim->b = m_b;
            dim->pos = v.point;
            if (!m_aligned) {
                // Horizontal measurement if the dim line is placed above or
                // below the points; vertical if placed to the side.
                const Vec2d d = m_b - m_a;
                const Vec2d off = v.point - (m_a + m_b) * 0.5;
                dim->axis = std::fabs(off.y) * std::fabs(d.x) >=
                                    std::fabs(off.x) * std::fabs(d.y)
                                ? Vec2d{1, 0}
                                : Vec2d{0, 1};
            }
            ctx.doc().beginTransaction(QLatin1String(name()));
            ctx.doc().addEntity(std::move(dim));
            ctx.doc().commitTransaction();
            return Step::done();
        }
        default:
            return Step::cancelled();
        }
    }

    void previewAt(CommandContext& ctx, const Vec2d& cursor, PrimitiveList& out) override
    {
        if (m_stage != 2)
            return;
        DimensionEntity ghost;
        ghost.kind = m_aligned ? DimensionEntity::Kind::Aligned
                               : DimensionEntity::Kind::Linear;
        ghost.a = m_a;
        ghost.b = m_b;
        ghost.pos = cursor;
        if (!m_aligned) {
            const Vec2d d = m_b - m_a;
            const Vec2d off = cursor - (m_a + m_b) * 0.5;
            ghost.axis = std::fabs(off.y) * std::fabs(d.x) >= std::fabs(off.x) * std::fabs(d.y)
                             ? Vec2d{1, 0}
                             : Vec2d{0, 1};
        }
        RenderContext rc;
        rc.doc = &ctx.doc();
        ghost.buildPrimitives(rc, out);
    }

private:
    bool m_aligned;
    int m_stage = 0;
    Vec2d m_a, m_b;
};

class DimAngularCommand : public Command {
public:
    const char* name() const override { return "DIMANGULAR"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point, QStringLiteral("Angle vertex:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind != InputValue::Kind::Point)
            return Step::cancelled();
        ctx.setLastPoint(v.point);
        switch (m_stage) {
        case 0:
            m_v = v.point;
            m_stage = 1;
            return Step::cont(InputKind::Point, QStringLiteral("First angle endpoint:"));
        case 1:
            m_p1 = v.point;
            m_stage = 2;
            return Step::cont(InputKind::Point, QStringLiteral("Second angle endpoint:"));
        case 2:
            m_p2 = v.point;
            m_stage = 3;
            return Step::cont(InputKind::Point, QStringLiteral("Dimension arc location:"));
        case 3: {
            auto dim = std::make_unique<DimensionEntity>();
            dim->kind = DimensionEntity::Kind::Angular;
            dim->a = m_v;
            dim->b = m_p1;
            dim->c = m_p2;
            dim->pos = v.point;
            ctx.doc().beginTransaction(QStringLiteral("DIMANGULAR"));
            ctx.doc().addEntity(std::move(dim));
            ctx.doc().commitTransaction();
            return Step::done();
        }
        default:
            return Step::cancelled();
        }
    }

private:
    int m_stage = 0;
    Vec2d m_v, m_p1, m_p2;
};

// Radius/diameter: pick a circle or arc, then the text position.
class DimRadialCommand : public Command {
public:
    explicit DimRadialCommand(bool diameter) : m_diameter(diameter) {}
    const char* name() const override { return m_diameter ? "DIMDIAMETER" : "DIMRADIUS"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point, QStringLiteral("Pick a circle or arc:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind != InputValue::Kind::Point)
            return Step::cancelled();
        if (m_stage == 0) {
            const EntityId id = hittest::pick(ctx.doc(), v.point, ctx.pickTolerance());
            const Entity* e = ctx.doc().entity(id);
            Vec2d center;
            double radius = 0;
            if (const auto* circle = dynamic_cast<const CircleEntity*>(e)) {
                center = circle->center();
                radius = circle->radius();
            } else if (const auto* arc = dynamic_cast<const ArcEntity*>(e)) {
                center = arc->center();
                radius = arc->radius();
            } else {
                ctx.info(QStringLiteral("pick a circle or an arc"));
                return Step::cont(InputKind::Point, QStringLiteral("Pick a circle or arc:"));
            }
            m_center = center;
            m_onCurve = center + (v.point - center).normalized() * radius;
            m_stage = 1;
            ctx.setLastPoint(v.point);
            return Step::cont(InputKind::Point, QStringLiteral("Text position:"));
        }
        auto dim = std::make_unique<DimensionEntity>();
        dim->kind = m_diameter ? DimensionEntity::Kind::Diameter
                               : DimensionEntity::Kind::Radius;
        dim->a = m_center;
        dim->b = m_onCurve;
        dim->pos = v.point;
        ctx.doc().beginTransaction(QLatin1String(name()));
        ctx.doc().addEntity(std::move(dim));
        ctx.doc().commitTransaction();
        return Step::done();
    }

private:
    bool m_diameter;
    int m_stage = 0;
    Vec2d m_center, m_onCurve;
};

// LEADER: points until Enter, then one line of text (Enter for none).
class LeaderCommand : public Command {
public:
    const char* name() const override { return "LEADER"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point, QStringLiteral("Leader start (arrow) point:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        if (!m_pointsDone) {
            if (v.kind == InputValue::Kind::Point) {
                m_points.push_back(v.point);
                ctx.setLastPoint(v.point);
                return Step::cont(InputKind::Point,
                                  QStringLiteral("Next point (Enter to finish):"));
            }
            if (v.kind == InputValue::Kind::Finish) {
                if (m_points.size() < 2)
                    return Step::cancelled();
                m_pointsDone = true;
                return Step::cont(InputKind::Text,
                                  QStringLiteral("Leader text (Enter for none):"));
            }
            return Step::cancelled();
        }
        QString content;
        if (v.kind == InputValue::Kind::Text)
            content = v.text;
        else if (v.kind != InputValue::Kind::Finish)
            return Step::cancelled();
        auto leader = std::make_unique<LeaderEntity>();
        leader->points = std::move(m_points);
        leader->text = content;
        ctx.doc().beginTransaction(QStringLiteral("LEADER"));
        ctx.doc().addEntity(std::move(leader));
        ctx.doc().commitTransaction();
        return Step::done();
    }

private:
    std::vector<Vec2d> m_points;
    bool m_pointsDone = false;
};

// HATCH pattern scale entity-ids
class HatchCommand : public Command {
public:
    const char* name() const override { return "HATCH"; }

    Step start(CommandContext& ctx) override
    {
        if (!ctx.selection().isEmpty()) {
            m_preselected = ctx.selection().ids();
            ctx.selection().clear();
        }
        return Step::cont(InputKind::Keyword,
                          QStringLiteral("Pattern [SOLID/ANSI31/ANSI37] <ANSI31>:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        switch (m_stage) {
        case 0:
            if (v.kind == InputValue::Kind::Keyword)
                m_pattern = v.text;
            else if (v.kind != InputValue::Kind::Finish)
                return Step::cancelled();
            m_stage = 1;
            return Step::cont(InputKind::Number, QStringLiteral("Pattern scale <1>:"));
        case 1:
            if (v.kind == InputValue::Kind::Number)
                m_scale = std::max(0.01, v.number);
            else if (v.kind != InputValue::Kind::Finish)
                return Step::cancelled();
            if (!m_preselected.empty())
                return build(ctx, m_preselected);
            m_stage = 2;
            return Step::cont(InputKind::EntitySet,
                              QStringLiteral("Select closed boundary entities:"));
        case 2:
            if (v.kind == InputValue::Kind::EntitySet)
                return build(ctx, v.entitySet);
            if (v.kind == InputValue::Kind::EntityRef) {
                m_picked.push_back(v.entityRef);
                return Step::cont(InputKind::EntitySet, QStringLiteral("Select boundaries:"));
            }
            if (v.kind == InputValue::Kind::Finish && !m_picked.empty())
                return build(ctx, m_picked);
            return Step::cancelled();
        default:
            return Step::cancelled();
        }
    }

private:
    Step build(CommandContext& ctx, const std::vector<EntityId>& ids)
    {
        auto hatch = std::make_unique<HatchEntity>();
        hatch->pattern = m_pattern;
        hatch->scale = m_scale;
        int rejected = 0;
        for (const EntityId id : ids) {
            const Entity* e = ctx.doc().entity(id);
            if (!e) {
                ++rejected;
                continue;
            }
            std::vector<Vec2d> ring;
            if (const auto* circle = dynamic_cast<const CircleEntity*>(e)) {
                flattenArc(circle->center(), circle->radius(), 0, 2 * M_PI, 0.05, ring);
                ring.pop_back(); // closed ring: drop duplicate end point
            } else if (const auto* pl = dynamic_cast<const PolylineEntity*>(e)) {
                if (!pl->isClosed()) {
                    ++rejected;
                    continue;
                }
                RenderContext rc;
                rc.chordTolerance = 0.05;
                PrimitiveList list;
                pl->buildPrimitives(rc, list);
                if (!list.strokes.empty())
                    ring = list.strokes[0].points;
            } else if (const auto* el = dynamic_cast<const EllipseEntity*>(e)) {
                if (!el->isFull()) {
                    ++rejected;
                    continue;
                }
                RenderContext rc;
                rc.chordTolerance = 0.05;
                PrimitiveList list;
                el->buildPrimitives(rc, list);
                if (!list.strokes.empty())
                    ring = list.strokes[0].points;
            } else {
                ++rejected;
                continue;
            }
            if (ring.size() >= 3)
                hatch->rings.push_back(std::move(ring));
        }
        if (hatch->rings.empty()) {
            ctx.info(QStringLiteral("no closed boundaries selected "
                                    "(circle / closed polyline / ellipse)"));
            return Step::cancelled();
        }
        if (rejected > 0)
            ctx.info(QStringLiteral("%1 entities were not closed boundaries").arg(rejected));
        ctx.doc().beginTransaction(QStringLiteral("HATCH"));
        ctx.doc().addEntity(std::move(hatch));
        ctx.doc().commitTransaction();
        return Step::done();
    }

    int m_stage = 0;
    QString m_pattern = QStringLiteral("ANSI31");
    double m_scale = 1.0;
    std::vector<EntityId> m_preselected, m_picked;
};

// DIMSTYLE: keyword/value pairs editing the current style.
// e.g. DIMSTYLE TH 5 AS 3 DEC 1
class DimStyleCommand : public Command {
public:
    const char* name() const override { return "DIMSTYLE"; }

    Step start(CommandContext& ctx) override
    {
        const DimStyle& s = ctx.doc().currentDimStyle();
        ctx.info(QStringLiteral("DimStyle '%1': TH=%2 AS=%3 EO=%4 EB=%5 GAP=%6 DEC=%7")
                     .arg(s.name)
                     .arg(s.textHeight)
                     .arg(s.arrowSize)
                     .arg(s.extOffset)
                     .arg(s.extBeyond)
                     .arg(s.textGap)
                     .arg(s.decimals));
        return Step::cont(InputKind::Keyword,
                          QStringLiteral("Set [TH/AS/EO/EB/GAP/DEC/SUF] (Enter to finish):"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        if (m_pendingKey.isEmpty()) {
            if (v.kind == InputValue::Kind::Finish)
                return Step::done();
            if (v.kind != InputValue::Kind::Keyword)
                return Step::cancelled();
            m_pendingKey = v.text;
            return Step::cont(m_pendingKey == QLatin1String("SUF") ? InputKind::Keyword
                                                                   : InputKind::Number,
                              QStringLiteral("Value for %1:").arg(m_pendingKey));
        }
        DimStyle s = ctx.doc().currentDimStyle();
        const QString key = m_pendingKey;
        m_pendingKey.clear();
        if (key == QLatin1String("SUF") && v.kind == InputValue::Kind::Keyword) {
            s.suffix = v.text == QLatin1String("NONE") ? QString() : v.text;
        } else if (v.kind == InputValue::Kind::Number) {
            if (key == QLatin1String("TH")) s.textHeight = std::max(0.1, v.number);
            else if (key == QLatin1String("AS")) s.arrowSize = std::max(0.1, v.number);
            else if (key == QLatin1String("EO")) s.extOffset = std::max(0.0, v.number);
            else if (key == QLatin1String("EB")) s.extBeyond = std::max(0.0, v.number);
            else if (key == QLatin1String("GAP")) s.textGap = std::max(0.0, v.number);
            else if (key == QLatin1String("DEC")) s.decimals = std::clamp(int(v.number), 0, 8);
        } else {
            return Step::cancelled();
        }
        ctx.doc().upsertDimStyle(s);
        return Step::cont(InputKind::Keyword,
                          QStringLiteral("Set [TH/AS/EO/EB/GAP/DEC/SUF] (Enter to finish):"));
    }

private:
    QString m_pendingKey;
};

std::unique_ptr<Command> makeDimLinear() { return std::make_unique<DimLinearCommand>(false); }
std::unique_ptr<Command> makeDimAligned() { return std::make_unique<DimLinearCommand>(true); }
std::unique_ptr<Command> makeDimRadius() { return std::make_unique<DimRadialCommand>(false); }
std::unique_ptr<Command> makeDimDiameter() { return std::make_unique<DimRadialCommand>(true); }

template <typename T>
std::unique_ptr<Command> make()
{
    return std::make_unique<T>();
}

} // namespace

void registerAnnotateCommands(CommandProcessor& p)
{
    p.registerCommand(&make<TextCommand>, {QStringLiteral("MTEXT"), QStringLiteral("T")});
    p.registerCommand(&make<TextEditCommand>, {QStringLiteral("ED"), QStringLiteral("DDEDIT")});
    p.registerCommand(&makeDimLinear, {QStringLiteral("DIMLIN"), QStringLiteral("DLI")});
    p.registerCommand(&makeDimAligned, {QStringLiteral("DAL")});
    p.registerCommand(&make<DimAngularCommand>, {QStringLiteral("DAN")});
    p.registerCommand(&makeDimRadius, {QStringLiteral("DRA")});
    p.registerCommand(&makeDimDiameter, {QStringLiteral("DDI")});
    p.registerCommand(&make<LeaderCommand>, {QStringLiteral("LE")});
    p.registerCommand(&make<HatchCommand>, {QStringLiteral("H")});
    p.registerCommand(&make<DimStyleCommand>, {QStringLiteral("DST")});
}

} // namespace viki
