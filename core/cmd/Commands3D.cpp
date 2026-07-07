#include "CommandProcessor.h"

#include "doc/EntityFactory.h"
#include "render/HitTest.h"
#include "solid/SolidEntity.h"
#include "solid/SolidOps.h"

// M7: WORKPLANE, EXTRUDE, REVOLVE, UNION/SUBTRACT/INTERSECT.
// The work plane lives in the CommandContext-free document? No — v1 keeps a
// per-command explicit approach: EXTRUDE/REVOLVE ask for the profile ids and
// use the current WORKPLANE Z offset stored in a process-wide setting on the
// context. Simplicity first; face planes arrive with the 3D GUI.

namespace viki {
namespace {

// The current work plane offset, per document — stored in a tiny registry
// keyed by document address (v1 pragmatism; becomes a Document field when
// face planes land).
double& workplaneZ(const Document& doc)
{
    static std::map<const Document*, double> zs;
    return zs[&doc];
}

// WORKPLANE XY | OFFSET z
class WorkplaneCommand : public Command {
public:
    const char* name() const override { return "WORKPLANE"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Keyword,
                          QStringLiteral("Work plane [XY / OFFSET] <XY>:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        if (m_awaitOffset) {
            if (v.kind != InputValue::Kind::Number)
                return Step::cancelled();
            workplaneZ(ctx.doc()) = v.number;
            ctx.info(QStringLiteral("work plane: XY at Z=%1").arg(v.number));
            return Step::done();
        }
        if (v.kind == InputValue::Kind::Keyword && v.text == QLatin1String("OFFSET")) {
            m_awaitOffset = true;
            return Step::cont(InputKind::Distance, QStringLiteral("Z offset:"));
        }
        workplaneZ(ctx.doc()) = 0.0;
        ctx.info(QStringLiteral("work plane: XY at Z=0"));
        return Step::done();
    }

private:
    bool m_awaitOffset = false;
};

// EXTRUDE height ids   (profile entities are consumed into the solid)
class ExtrudeCommand : public Command {
public:
    const char* name() const override { return "EXTRUDE"; }

    Step start(CommandContext& ctx) override
    {
        if (!ctx.selection().isEmpty()) {
            m_ids = ctx.selection().ids();
            ctx.selection().clear();
        }
        return Step::cont(InputKind::Distance, QStringLiteral("Extrusion height:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        if (!m_haveHeight) {
            if (v.kind != InputValue::Kind::Number)
                return Step::cancelled();
            m_height = v.number;
            m_haveHeight = true;
            if (!m_ids.empty())
                return build(ctx);
            return Step::cont(InputKind::EntitySet,
                              QStringLiteral("Select profile entities:"));
        }
        switch (v.kind) {
        case InputValue::Kind::EntitySet:
            m_ids = v.entitySet;
            return build(ctx);
        case InputValue::Kind::EntityRef:
            m_ids.push_back(v.entityRef);
            return Step::cont(InputKind::EntitySet, QStringLiteral("Select profiles:"));
        case InputValue::Kind::Finish:
            return m_ids.empty() ? Step::cancelled() : build(ctx);
        default:
            return Step::cancelled();
        }
    }

private:
    Step build(CommandContext& ctx)
    {
        const WorkPlane plane{workplaneZ(ctx.doc())};
        const auto wires = solidops::wiresFromEntities(ctx.doc(), m_ids, plane);
        if (!wires.ok) {
            ctx.info(wires.message);
            return Step::cancelled();
        }
        const auto solid = solidops::extrudeWires(wires.wires, m_height);
        if (!solid.ok) {
            ctx.info(solid.message);
            return Step::cancelled();
        }
        ctx.doc().beginTransaction(QStringLiteral("EXTRUDE"));
        for (const EntityId id : m_ids)
            ctx.doc().removeEntity(id);
        const EntityId sid =
            ctx.doc().addEntity(std::make_unique<SolidEntity>(solid.shape));
        ctx.doc().commitTransaction();
        ctx.info(QStringLiteral("solid %1 created (height %2)").arg(sid).arg(m_height));
        return Step::done();
    }

    std::vector<EntityId> m_ids;
    bool m_haveHeight = false;
    double m_height = 10.0;
};

// REVOLVE angle_deg axis_p1 axis_p2 ids
class RevolveCommand : public Command {
public:
    const char* name() const override { return "REVOLVE"; }

    Step start(CommandContext& ctx) override
    {
        if (!ctx.selection().isEmpty()) {
            m_ids = ctx.selection().ids();
            ctx.selection().clear();
        }
        return Step::cont(InputKind::Number,
                          QStringLiteral("Revolution angle (degrees) <360>:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        switch (m_stage) {
        case 0:
            if (v.kind == InputValue::Kind::Number)
                m_angle = std::clamp(v.number, 1.0, 360.0) * M_PI / 180.0;
            else if (v.kind != InputValue::Kind::Finish)
                return Step::cancelled();
            m_stage = 1;
            return Step::cont(InputKind::Point, QStringLiteral("Axis first point:"));
        case 1:
            if (v.kind != InputValue::Kind::Point)
                return Step::cancelled();
            m_axisA = v.point;
            m_stage = 2;
            ctx.setLastPoint(v.point);
            return Step::cont(InputKind::Point, QStringLiteral("Axis second point:"));
        case 2:
            if (v.kind != InputValue::Kind::Point)
                return Step::cancelled();
            m_axisB = v.point;
            m_stage = 3;
            if (!m_ids.empty())
                return build(ctx);
            return Step::cont(InputKind::EntitySet,
                              QStringLiteral("Select profile entities:"));
        case 3:
            if (v.kind == InputValue::Kind::EntitySet) {
                m_ids = v.entitySet;
                return build(ctx);
            }
            if (v.kind == InputValue::Kind::EntityRef) {
                m_ids.push_back(v.entityRef);
                return Step::cont(InputKind::EntitySet, QStringLiteral("Select profiles:"));
            }
            if (v.kind == InputValue::Kind::Finish && !m_ids.empty())
                return build(ctx);
            return Step::cancelled();
        default:
            return Step::cancelled();
        }
    }

private:
    Step build(CommandContext& ctx)
    {
        const WorkPlane plane{workplaneZ(ctx.doc())};
        const auto wires = solidops::wiresFromEntities(ctx.doc(), m_ids, plane);
        if (!wires.ok) {
            ctx.info(wires.message);
            return Step::cancelled();
        }
        const auto solid =
            solidops::revolveWires(wires.wires, m_axisA, m_axisB, m_angle, plane);
        if (!solid.ok) {
            ctx.info(solid.message);
            return Step::cancelled();
        }
        ctx.doc().beginTransaction(QStringLiteral("REVOLVE"));
        for (const EntityId id : m_ids)
            ctx.doc().removeEntity(id);
        const EntityId sid =
            ctx.doc().addEntity(std::make_unique<SolidEntity>(solid.shape));
        ctx.doc().commitTransaction();
        ctx.info(QStringLiteral("solid %1 created").arg(sid));
        return Step::done();
    }

    int m_stage = 0;
    double m_angle = 2.0 * M_PI;
    Vec2d m_axisA, m_axisB;
    std::vector<EntityId> m_ids;
};

// UNION/SUBTRACT/INTERSECT id-a id-b  → replaces both with the result.
class BooleanCommand : public Command {
public:
    explicit BooleanCommand(solidops::BoolOp op) : m_op(op) {}
    const char* name() const override
    {
        switch (m_op) {
        case solidops::BoolOp::Union: return "UNION";
        case solidops::BoolOp::Subtract: return "SUBTRACT";
        case solidops::BoolOp::Intersect: return "INTERSECT";
        }
        return "UNION";
    }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::EntitySet,
                          QStringLiteral("Select two solids (ids):"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind != InputValue::Kind::EntitySet || v.entitySet.size() != 2)
            return Step::cancelled();
        auto* sa = dynamic_cast<SolidEntity*>(ctx.doc().entity(v.entitySet[0]));
        auto* sb = dynamic_cast<SolidEntity*>(ctx.doc().entity(v.entitySet[1]));
        if (!sa || !sb) {
            ctx.info(QStringLiteral("both ids must be solids"));
            return Step::cancelled();
        }
        const auto out = solidops::booleanOp(sa->shape(), sb->shape(), m_op);
        if (!out.ok) {
            ctx.info(out.message);
            return Step::cancelled();
        }
        ctx.doc().beginTransaction(QLatin1String(name()));
        ctx.doc().removeEntity(v.entitySet[0]);
        ctx.doc().removeEntity(v.entitySet[1]);
        const EntityId sid =
            ctx.doc().addEntity(std::make_unique<SolidEntity>(out.shape));
        ctx.doc().commitTransaction();
        ctx.info(QStringLiteral("solid %1 created").arg(sid));
        return Step::done();
    }

private:
    solidops::BoolOp m_op;
};

std::unique_ptr<Command> makeUnion()
{
    return std::make_unique<BooleanCommand>(solidops::BoolOp::Union);
}
std::unique_ptr<Command> makeSubtract()
{
    return std::make_unique<BooleanCommand>(solidops::BoolOp::Subtract);
}
std::unique_ptr<Command> makeIntersect()
{
    return std::make_unique<BooleanCommand>(solidops::BoolOp::Intersect);
}

template <typename T>
std::unique_ptr<Command> make()
{
    return std::make_unique<T>();
}

} // namespace

void registerSolidCommands(CommandProcessor& p)
{
    p.registerCommand(&make<WorkplaneCommand>, {QStringLiteral("WP")});
    p.registerCommand(&make<ExtrudeCommand>, {QStringLiteral("EXT")});
    p.registerCommand(&make<RevolveCommand>, {QStringLiteral("REV")});
    p.registerCommand(&makeUnion);
    p.registerCommand(&makeSubtract, {QStringLiteral("SUB")});
    p.registerCommand(&makeIntersect, {QStringLiteral("INT")});
}

} // namespace viki
