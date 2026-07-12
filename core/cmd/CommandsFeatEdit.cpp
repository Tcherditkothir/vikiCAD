#include "CommandProcessor.h"

#include "solid/FeatureParams.h"
#include "solid/SolidEntity.h"

#include <cmath>

// FEATEDIT — the headless feature editor. The GUI edits hole/shell/extrude
// parameters through the Properties panel; agents get the exact same edit
// path (featureparams::set + SolidEntity::regenerateFeatures, one document
// transaction) over the command line / IPC:
//
//   FEATEDIT <param> <value> <nodeIndex> <solidId>
//       param in {diameter, depth, centerx, centery, height, thickness}
//       (centerx/centery map to featureparams "center x"/"center y",
//        which MOVES the bore — signed values are legal there).
//   FEATEDIT LIST <solidId>
//       prints every editable parameter: "<label> = <value>" per line,
//       e.g. "hole 1: diameter = 4.0" — the discovery half of the verb.
//
// Grammar keeps ALL numeric parameters before the entity selection (the
// EntitySet gulp rule) and the id last, INSPECT-style.

namespace viki {
namespace {

QString num(double v)
{
    // Fixed one decimal, same contract as INSPECT: stable for agents to parse.
    return QString::number(v, 'f', 1);
}

// Uppercased command keyword -> featureparams name. Empty = unknown.
QString paramName(const QString& kw)
{
    if (kw == QLatin1String("DIAMETER"))
        return QStringLiteral("diameter");
    if (kw == QLatin1String("DEPTH"))
        return QStringLiteral("depth");
    if (kw == QLatin1String("CENTERX"))
        return QStringLiteral("center x");
    if (kw == QLatin1String("CENTERY"))
        return QStringLiteral("center y");
    if (kw == QLatin1String("HEIGHT"))
        return QStringLiteral("height");
    if (kw == QLatin1String("THICKNESS"))
        return QStringLiteral("thickness");
    return {};
}

class FeatEditCommand : public Command {
public:
    const char* name() const override { return "FEATEDIT"; }

    Step start(CommandContext&) override
    {
        return Step::cont(
            InputKind::Keyword,
            QStringLiteral("Parameter [Diameter/Depth/CenterX/CenterY/"
                           "Height/Thickness/List]:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        switch (m_st) {
        case St::Param: {
            if (v.kind != InputValue::Kind::Keyword)
                return Step::cancelled();
            if (v.text == QLatin1String("LIST") || v.text == QLatin1String("L")) {
                m_list = true;
                m_st = St::Solid;
                return Step::cont(InputKind::EntitySet,
                                  QStringLiteral("Select solid (id):"));
            }
            m_param = paramName(v.text);
            if (m_param.isEmpty()) {
                ctx.info(QStringLiteral(
                    "unknown parameter '%1' — expected diameter, depth, "
                    "centerx, centery, height, thickness or LIST")
                             .arg(v.text.toLower()));
                return Step::cancelled();
            }
            m_st = St::Value;
            return Step::cont(InputKind::Number,
                              QStringLiteral("New value (mm):"));
        }
        case St::Value:
            // Raw Number, not Distance: centre coordinates are signed
            // positions and featureparams values are always plain mm.
            if (v.kind != InputValue::Kind::Number)
                return Step::cancelled();
            m_value = v.number;
            m_st = St::Node;
            return Step::cont(InputKind::Number,
                              QStringLiteral("Feature node index:"));
        case St::Node: {
            if (v.kind != InputValue::Kind::Number)
                return Step::cancelled();
            if (v.number < 0 || std::floor(v.number) != v.number) {
                ctx.info(QStringLiteral(
                    "node index must be a non-negative integer"));
                return Step::cancelled();
            }
            m_node = int(v.number);
            m_st = St::Solid;
            return Step::cont(InputKind::EntitySet,
                              QStringLiteral("Select solid (id):"));
        }
        case St::Solid:
            return applyTo(ctx, v);
        }
        return Step::cancelled();
    }

private:
    Step applyTo(CommandContext& ctx, const InputValue& v)
    {
        EntityId id = kInvalidEntityId;
        if (v.kind == InputValue::Kind::EntitySet && v.entitySet.size() == 1)
            id = v.entitySet[0];
        else if (v.kind == InputValue::Kind::EntityRef)
            id = v.entityRef;
        if (id == kInvalidEntityId)
            return Step::cancelled();
        const auto* solid = dynamic_cast<const SolidEntity*>(ctx.doc().entity(id));
        if (!solid) {
            ctx.info(QStringLiteral("that id is not a solid"));
            return Step::cancelled();
        }
        if (!solid->features) {
            ctx.info(QStringLiteral("solid %1 has no feature history — "
                                    "nothing editable")
                         .arg(id));
            return Step::cancelled();
        }

        if (m_list) {
            const auto params = featureparams::list(*solid->features);
            ctx.info(QStringLiteral("solid %1: %2 editable parameter(s)")
                         .arg(id)
                         .arg(params.size()));
            for (const auto& p : params)
                ctx.info(QStringLiteral("%1 = %2").arg(p.label, num(p.value)));
            return Step::done();
        }

        // The Properties-panel path, verbatim: setter + regenerate inside ONE
        // transaction; any failure rolls the document back untouched.
        ctx.doc().beginTransaction(QLatin1String(name()));
        auto* mut = static_cast<SolidEntity*>(ctx.doc().beginModify(id));
        if (!featureparams::set(*mut->features, m_node, m_param, m_value)) {
            ctx.doc().endModify(id);
            ctx.doc().rollbackTransaction();
            ctx.info(QStringLiteral(
                "solid %1 has no editable '%2' on node %3 (wrong node kind, "
                "index out of range, or non-positive length)")
                         .arg(id)
                         .arg(m_param)
                         .arg(m_node));
            return Step::cancelled();
        }
        if (!mut->regenerateFeatures()) {
            ctx.doc().endModify(id);
            ctx.doc().rollbackTransaction();
            ctx.info(QStringLiteral(
                "regeneration failed for %1 = %2 on node %3 — solid %4 "
                "left unchanged")
                         .arg(m_param, num(m_value))
                         .arg(m_node)
                         .arg(id));
            return Step::cancelled();
        }
        ctx.doc().endModify(id);
        ctx.doc().commitTransaction();
        ctx.info(QStringLiteral("solid %1: %2 = %3 on node %4")
                     .arg(id)
                     .arg(m_param, num(m_value))
                     .arg(m_node));
        return Step::done();
    }

    enum class St { Param, Value, Node, Solid };
    St m_st = St::Param;
    bool m_list = false;
    QString m_param;
    double m_value = 0.0;
    int m_node = -1;
};

std::unique_ptr<Command> makeFeatEdit()
{
    return std::make_unique<FeatEditCommand>();
}

} // namespace

void registerFeatEditCommands(CommandProcessor& p)
{
    p.registerCommand(&makeFeatEdit);
}

} // namespace viki
