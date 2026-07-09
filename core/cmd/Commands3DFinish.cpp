#include "CommandProcessor.h"

#include "solid/SolidEntity.h"
#include "solid/SolidOps.h"

// M8: FILLET3D / CHAMFER3D. v1 applies to ALL edges of the solid (per-edge
// picking arrives with AIS selection work in the 3D view — documented).

namespace viki {
namespace {

class Fillet3DCommand : public Command {
public:
    explicit Fillet3DCommand(bool chamfer) : m_chamfer(chamfer) {}
    const char* name() const override { return m_chamfer ? "CHAMFER3D" : "FILLET3D"; }

    Step start(CommandContext&) override
    {
        // Radius first so the EntitySet token gulp cannot swallow it.
        return Step::cont(InputKind::Distance,
                          m_chamfer ? QStringLiteral("Chamfer distance:")
                                    : QStringLiteral("Fillet radius:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        if (m_value <= 0) {
            if (v.kind != InputValue::Kind::Number || v.number <= kGeomTol)
                return Step::cancelled();
            m_value = v.number;
            return Step::cont(InputKind::EntitySet, QStringLiteral("Select solid (id):"));
        }
        if (v.kind != InputValue::Kind::EntitySet || v.entitySet.size() != 1)
            return Step::cancelled();
        if (!dynamic_cast<SolidEntity*>(ctx.doc().entity(v.entitySet[0]))) {
            ctx.info(QStringLiteral("that id is not a solid"));
            return Step::cancelled();
        }
        m_id = v.entitySet[0];

        auto* solid = dynamic_cast<SolidEntity*>(ctx.doc().entity(m_id));
        // v1 applies to ALL edges (n <= 0 = all in TopExp order). Per-edge
        // picking rides in via `solidops::filletEdges`/`chamferEdges` once the
        // 3D view exposes a picked-edge selection.
        const auto out = m_chamfer
                             ? solidops::chamferFirstNEdges(solid->shape(), 0, m_value)
                             : solidops::filletFirstNEdges(solid->shape(), 0, m_value);
        if (!out.ok || out.shape.IsNull()) {
            ctx.info(out.message.isEmpty()
                         ? QStringLiteral("%1 failed").arg(QLatin1String(name()))
                         : out.message);
            return Step::cancelled();
        }
        ctx.doc().beginTransaction(QLatin1String(name()));
        auto* mut = static_cast<SolidEntity*>(ctx.doc().beginModify(m_id));
        mut->setShape(out.shape);
        ctx.doc().endModify(m_id);
        ctx.doc().commitTransaction();
        ctx.info(QStringLiteral("%1 applied to all edges").arg(QLatin1String(name())));
        return Step::done();
    }

private:
    bool m_chamfer;
    EntityId m_id = kInvalidEntityId;
    double m_value = 0.0;
};

// SHELL: hollow a solid to a wall thickness. v1 shells all around (no open
// face); face-picking to open a side arrives with AIS selection in the 3D view.
class ShellCommand : public Command {
public:
    const char* name() const override { return "SHELL"; }

    Step start(CommandContext&) override
    {
        // Thickness first so the EntitySet token gulp cannot swallow it.
        return Step::cont(InputKind::Distance, QStringLiteral("Wall thickness:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        if (m_thickness <= 0) {
            if (v.kind != InputValue::Kind::Number || v.number <= kGeomTol)
                return Step::cancelled();
            m_thickness = v.number;
            return Step::cont(InputKind::EntitySet,
                              QStringLiteral("Select solid (id):"));
        }
        if (v.kind != InputValue::Kind::EntitySet || v.entitySet.size() != 1)
            return Step::cancelled();
        auto* solid = dynamic_cast<SolidEntity*>(ctx.doc().entity(v.entitySet[0]));
        if (!solid) {
            ctx.info(QStringLiteral("that id is not a solid"));
            return Step::cancelled();
        }
        m_id = v.entitySet[0];

        const auto out = solidops::shellSolid(solid->shape(), m_thickness);
        if (!out.ok || out.shape.IsNull()) {
            ctx.info(out.message.isEmpty() ? QStringLiteral("shell failed")
                                           : out.message);
            return Step::cancelled();
        }
        ctx.doc().beginTransaction(QStringLiteral("SHELL"));
        auto* mut = static_cast<SolidEntity*>(ctx.doc().beginModify(m_id));
        mut->setShape(out.shape);
        ctx.doc().endModify(m_id);
        ctx.doc().commitTransaction();
        ctx.info(QStringLiteral("shelled to %1 mm wall").arg(m_thickness));
        return Step::done();
    }

private:
    EntityId m_id = kInvalidEntityId;
    double m_thickness = 0.0;
};

std::unique_ptr<Command> makeShell()
{
    return std::make_unique<ShellCommand>();
}

std::unique_ptr<Command> makeFillet3D()
{
    return std::make_unique<Fillet3DCommand>(false);
}
std::unique_ptr<Command> makeChamfer3D()
{
    return std::make_unique<Fillet3DCommand>(true);
}

} // namespace

void registerSolidFinishCommands(CommandProcessor& p)
{
    p.registerCommand(&makeFillet3D, {QStringLiteral("F3D")});
    p.registerCommand(&makeChamfer3D, {QStringLiteral("CH3D")});
    p.registerCommand(&makeShell, {QStringLiteral("SH")});
}

} // namespace viki
