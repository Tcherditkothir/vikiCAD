#include "CommandProcessor.h"

#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>

#include "solid/SolidEntity.h"

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
        TopoDS_Shape result;
        try {
            if (m_chamfer) {
                BRepFilletAPI_MakeChamfer op(solid->shape());
                for (TopExp_Explorer exp(solid->shape(), TopAbs_EDGE); exp.More();
                     exp.Next())
                    op.Add(m_value, TopoDS::Edge(exp.Current()));
                op.Build();
                if (op.IsDone())
                    result = op.Shape();
            } else {
                BRepFilletAPI_MakeFillet op(solid->shape());
                for (TopExp_Explorer exp(solid->shape(), TopAbs_EDGE); exp.More();
                     exp.Next())
                    op.Add(m_value, TopoDS::Edge(exp.Current()));
                op.Build();
                if (op.IsDone())
                    result = op.Shape();
            }
        } catch (...) {
            // OCCT throws on infeasible radii; fall through to the message.
        }
        if (result.IsNull()) {
            ctx.info(QStringLiteral("%1 failed — radius/distance too large for "
                                    "this geometry?")
                         .arg(QLatin1String(name())));
            return Step::cancelled();
        }
        ctx.doc().beginTransaction(QLatin1String(name()));
        auto* mut = static_cast<SolidEntity*>(ctx.doc().beginModify(m_id));
        mut->setShape(result);
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
}

} // namespace viki
