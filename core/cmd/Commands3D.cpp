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

// WORKPLANE XY | OFFSET z   (the sketch-on-face plane is set from the 3D view)
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
            documentWorkplane(ctx.doc()) = WorkPlane{gp_Pnt(0, 0, v.number),
                                                     gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)};
            ctx.info(QStringLiteral("work plane: XY at Z=%1").arg(v.number));
            return Step::done();
        }
        if (v.kind == InputValue::Kind::Keyword && v.text == QLatin1String("OFFSET")) {
            m_awaitOffset = true;
            return Step::cont(InputKind::Distance, QStringLiteral("Z offset:"));
        }
        documentWorkplane(ctx.doc()) = WorkPlane{}; // world XY
        ctx.info(QStringLiteral("work plane: XY at Z=0"));
        return Step::done();
    }

private:
    bool m_awaitOffset = false;
};

// EXTRUDE height [New/Join/Cut/Symmetric] (target) ids
// Numeric height first (before any selection), then the mode keyword. For
// Join/Cut a target solid is picked; then the profile entities are consumed.
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
        switch (m_stage) {
        case 0: // height
            if (v.kind != InputValue::Kind::Number)
                return Step::cancelled();
            m_height = v.number;
            m_stage = 1;
            return Step::cont(InputKind::Keyword,
                              QStringLiteral("Mode [New/Join/Cut/Symmetric] <New>:"));
        case 1: // mode keyword (Finish / Enter keeps the default New)
            if (v.kind == InputValue::Kind::Keyword) {
                const QString k = v.text.toUpper();
                // A bare id at the mode prompt (e.g. "EXTRUDE 20 1") means the
                // user skipped the mode: default New and this is the first
                // profile id. Keep the pre-modes call form working.
                bool digits = !k.isEmpty();
                for (const QChar c : k)
                    digits = digits && c.isDigit();
                if (digits) {
                    m_mode = solidops::ExtrudeMode::NewBody;
                    m_ids.push_back(k.toLongLong());
                    m_stage = 3;
                    return Step::cont(InputKind::EntitySet,
                                      QStringLiteral("Select profiles:"));
                }
                if (k == QLatin1String("NEW") || k == QLatin1String("N"))
                    m_mode = solidops::ExtrudeMode::NewBody;
                else if (k == QLatin1String("JOIN") || k == QLatin1String("J"))
                    m_mode = solidops::ExtrudeMode::Join;
                else if (k == QLatin1String("CUT") || k == QLatin1String("C"))
                    m_mode = solidops::ExtrudeMode::Cut;
                else if (k == QLatin1String("SYMMETRIC") || k == QLatin1String("S"))
                    m_mode = solidops::ExtrudeMode::Symmetric;
                else
                    return Step::cancelled();
            } else if (v.kind != InputValue::Kind::Finish) {
                return Step::cancelled();
            }
            if (m_mode == solidops::ExtrudeMode::Join ||
                m_mode == solidops::ExtrudeMode::Cut) {
                m_stage = 2; // ask for the target next
                return Step::cont(InputKind::EntitySet,
                                  QStringLiteral("Pick target solid:"));
            }
            m_stage = 3;
            if (!m_ids.empty())
                return build(ctx);
            return Step::cont(InputKind::EntitySet,
                              QStringLiteral("Select profile entities:"));
        case 2: // target solid (Join/Cut only)
            if (v.kind == InputValue::Kind::EntityRef)
                m_target = v.entityRef;
            else if (v.kind == InputValue::Kind::EntitySet && !v.entitySet.empty())
                m_target = v.entitySet.front();
            else
                return Step::cancelled();
            m_stage = 3;
            if (!m_ids.empty())
                return build(ctx);
            return Step::cont(InputKind::EntitySet,
                              QStringLiteral("Select profile entities:"));
        case 3: // profile entities
            switch (v.kind) {
            case InputValue::Kind::EntitySet:
                // Append: a leading id may already have been captured at the
                // mode prompt (legacy "EXTRUDE h id id ..." form).
                m_ids.insert(m_ids.end(), v.entitySet.begin(), v.entitySet.end());
                return build(ctx);
            case InputValue::Kind::EntityRef:
                m_ids.push_back(v.entityRef);
                return Step::cont(InputKind::EntitySet, QStringLiteral("Select profiles:"));
            case InputValue::Kind::Finish:
                return m_ids.empty() ? Step::cancelled() : build(ctx);
            default:
                return Step::cancelled();
            }
        default:
            return Step::cancelled();
        }
    }

private:
    Step build(CommandContext& ctx)
    {
        const WorkPlane plane = documentWorkplane(ctx.doc());
        const auto wires = solidops::wiresFromEntities(ctx.doc(), m_ids, plane);
        if (!wires.ok) {
            ctx.info(wires.message);
            return Step::cancelled();
        }
        TopoDS_Shape targetShape;
        if (m_mode == solidops::ExtrudeMode::Join ||
            m_mode == solidops::ExtrudeMode::Cut) {
            auto* t = dynamic_cast<SolidEntity*>(ctx.doc().entity(m_target));
            if (!t) {
                ctx.info(QStringLiteral("target must be a solid"));
                return Step::cancelled();
            }
            targetShape = t->shape();
        }
        const auto solid =
            solidops::extrudeWires(wires.wires, m_height, plane, m_mode, targetShape);
        if (!solid.ok) {
            ctx.info(solid.message);
            return Step::cancelled();
        }
        ctx.doc().beginTransaction(QStringLiteral("EXTRUDE"));
        for (const EntityId id : m_ids)
            ctx.doc().removeEntity(id);
        if (m_mode == solidops::ExtrudeMode::Join ||
            m_mode == solidops::ExtrudeMode::Cut)
            ctx.doc().removeEntity(m_target);
        const EntityId sid =
            ctx.doc().addEntity(std::make_unique<SolidEntity>(solid.shape));
        ctx.doc().commitTransaction();
        ctx.info(QStringLiteral("solid %1 created (height %2)").arg(sid).arg(m_height));
        return Step::done();
    }

    int m_stage = 0;
    std::vector<EntityId> m_ids;
    double m_height = 10.0;
    solidops::ExtrudeMode m_mode = solidops::ExtrudeMode::NewBody;
    EntityId m_target = 0;
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
        const WorkPlane plane = documentWorkplane(ctx.doc());
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

// HOLE diameter (depth | T=through) center-point solid-id
// Numeric params come first (the greedy EntitySet must not eat them).
class HoleCommand : public Command {
public:
    const char* name() const override { return "HOLE"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Distance, QStringLiteral("Hole diameter:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        switch (m_stage) {
        case 0: // diameter
            if (v.kind != InputValue::Kind::Number || v.number <= 0)
                return Step::cancelled();
            m_diameter = v.number;
            m_stage = 1;
            return Step::cont(InputKind::Distance,
                              QStringLiteral("Depth or [T=through]:"));
        case 1: // depth or Through
            if (v.kind == InputValue::Kind::Keyword && v.text == QLatin1String("T")) {
                m_through = true;
            } else if (v.kind == InputValue::Kind::Number && v.number > 0) {
                m_depth = v.number;
            } else {
                return Step::cancelled();
            }
            m_stage = 2;
            return Step::cont(InputKind::Point, QStringLiteral("Hole center:"));
        case 2: // center point
            if (v.kind != InputValue::Kind::Point)
                return Step::cancelled();
            m_center = v.point;
            ctx.setLastPoint(v.point);
            m_stage = 3;
            return Step::cont(InputKind::EntitySet, QStringLiteral("Pick target solid:"));
        case 3: // target solid
            if (v.kind == InputValue::Kind::EntityRef)
                return build(ctx, v.entityRef);
            if (v.kind == InputValue::Kind::EntitySet && !v.entitySet.empty())
                return build(ctx, v.entitySet.front());
            return Step::cancelled();
        default:
            return Step::cancelled();
        }
    }

private:
    Step build(CommandContext& ctx, EntityId target)
    {
        auto* solid = dynamic_cast<SolidEntity*>(ctx.doc().entity(target));
        if (!solid) {
            ctx.info(QStringLiteral("target must be a solid"));
            return Step::cancelled();
        }
        const WorkPlane plane = documentWorkplane(ctx.doc());
        const auto out = solidops::makeHole(solid->shape(), plane, m_center,
                                            m_diameter, m_depth, m_through);
        if (!out.ok) {
            ctx.info(out.message);
            return Step::cancelled();
        }
        ctx.doc().beginTransaction(QStringLiteral("HOLE"));
        ctx.doc().removeEntity(target);
        const EntityId sid =
            ctx.doc().addEntity(std::make_unique<SolidEntity>(out.shape));
        ctx.doc().commitTransaction();
        ctx.info(QStringLiteral("hole (d=%1) in solid %2").arg(m_diameter).arg(sid));
        return Step::done();
    }

    int m_stage = 0;
    double m_diameter = 5.0;
    double m_depth = 10.0;
    bool m_through = false;
    Vec2d m_center;
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

// MEASURE3D  id-a id-b  — reports the minimum 3D distance (mm) between two
// solids. Read-only: no transaction, no mutation. The distance is surfaced via
// ctx.info (picked up by the CLI/GUI message channel).
class Measure3DCommand : public Command {
public:
    const char* name() const override { return "MEASURE3D"; }

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
        const double d = solidops::minDistance(sa->shape(), sb->shape());
        if (d < 0.0) {
            ctx.info(QStringLiteral("distance computation failed"));
            return Step::cancelled();
        }
        ctx.info(QStringLiteral("min distance = %1 mm").arg(d, 0, 'g', 12));
        return Step::done();
    }
};

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
    p.registerCommand(&make<HoleCommand>, {QStringLiteral("HO")});
    p.registerCommand(&make<Measure3DCommand>, {QStringLiteral("M3D")});
}

} // namespace viki
