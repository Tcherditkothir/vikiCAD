#include "CommandProcessor.h"

#include <QJsonArray>
#include <QJsonObject>

#include "io/QueryJson.h"
#include "solid/SolidEntity.h"

// DESCRIBE (alias DESC) — the text half of "understand the model". No args
// = the whole document; one solid id = the document line + that solid only.
// Every line is computed on the spot from the SAME data the machine twin
// (queryjson::describeJson, i.e. `query describe`) serves, then rendered
// grep-friendly at fixed one-decimal precision:
//   document: units=mm entities=2 layers=1
//   solid 3: volume=3874.3 mm3 area=925.7 mm2 bbox=(0.0,0.0,0.0)-(20.0,20.0,10.0) centroid=(10.0,10.0,5.0)
//     base 0
//     hole 1 d=4 through @(10,10)
//   sketch 1 'Sketch 1': origin=(0.0,0.0,0.0) normal=(0.0,0.0,1.0) entities=1
//   layer '0': 2d=1 circle=1

namespace viki {
namespace {

QString num(double v)
{
    // Fixed one decimal, like INSPECT: stable to parse, precise enough.
    // OCCT bounding boxes carry a tiny epsilon gap, so a mathematical 0
    // can land at -1e-7 and print as "-0.0" — normalise it away.
    const QString s = QString::number(v, 'f', 1);
    return s == QLatin1String("-0.0") ? QStringLiteral("0.0") : s;
}

QString shortNum(double v)
{
    // Parameter values keep the featureparams flavour: 4 not 4.0.
    return QString::number(v);
}

QString triple(const QJsonArray& a)
{
    return QStringLiteral("(%1,%2,%3)").arg(num(a.at(0).toDouble()),
                                            num(a.at(1).toDouble()),
                                            num(a.at(2).toDouble()));
}

// featureparams-style one-liner for EVERY node of a tree (the JSON twin
// keeps only param-bearing nodes; the text view shows the full history).
QString featureLabel(int index, const FeatureNode& n)
{
    switch (n.kind) {
    case FeatureKind::Sketch:
        return QStringLiteral("sketch %1 profiles=%2")
            .arg(index)
            .arg(n.profiles.size());
    case FeatureKind::Extrude:
        return QStringLiteral("extrude %1 h=%2").arg(index).arg(
            shortNum(n.height));
    case FeatureKind::BaseShape:
        return QStringLiteral("base %1").arg(index);
    case FeatureKind::Hole: {
        const QString depth = n.through
                                  ? QStringLiteral("through")
                                  : QStringLiteral("depth=%1").arg(
                                        shortNum(n.depth));
        return QStringLiteral("hole %1 d=%2 %3 @(%4,%5)")
            .arg(index)
            .arg(shortNum(n.diameter), depth, shortNum(n.holeCenter.x),
                 shortNum(n.holeCenter.y));
    }
    case FeatureKind::Shell:
        return QStringLiteral("shell %1 t=%2").arg(index).arg(
            shortNum(n.thickness));
    }
    return QStringLiteral("feature %1").arg(index);
}

void describe(CommandContext& ctx, EntityId only)
{
    const Document& doc = ctx.doc();
    const QJsonObject d = queryjson::describeJson(doc);

    ctx.info(QStringLiteral("document: units=%1 entities=%2 layers=%3")
                 .arg(d[QStringLiteral("units")].toString())
                 .arg(d[QStringLiteral("entityCount")].toInteger())
                 .arg(d[QStringLiteral("layerCount")].toInteger()));

    for (const QJsonValue sv : d[QStringLiteral("solids")].toArray()) {
        const QJsonObject s = sv.toObject();
        const EntityId id = EntityId(s[QStringLiteral("id")].toInteger());
        if (only != kInvalidEntityId && id != only)
            continue;
        QString line = QStringLiteral("solid %1").arg(id);
        const QString comp = s[QStringLiteral("component")].toString();
        if (!comp.isEmpty())
            line += QStringLiteral(" '%1'").arg(comp);
        line += QStringLiteral(": volume=%1 mm3 area=%2 mm2")
                    .arg(num(s[QStringLiteral("volume")].toDouble()),
                         num(s[QStringLiteral("area")].toDouble()));
        const QJsonObject bbox = s[QStringLiteral("bbox")].toObject();
        line += QStringLiteral(" bbox=%1-%2 centroid=%3")
                    .arg(triple(bbox[QStringLiteral("min")].toArray()),
                         triple(bbox[QStringLiteral("max")].toArray()),
                         triple(s[QStringLiteral("centroid")].toArray()));
        ctx.info(line);
        // Feature lines come straight from the tree: the text view lists
        // EVERY node (base/sketch included), not just the editable ones.
        const auto* solid = dynamic_cast<const SolidEntity*>(doc.entity(id));
        if (solid && solid->features)
            for (int i = 0; i < solid->features->count(); ++i)
                ctx.info(QStringLiteral("  ") +
                         featureLabel(i, solid->features->nodeAt(i)));
    }

    if (only != kInvalidEntityId)
        return; // single-solid scope: no sketch/layer inventory

    for (const QJsonValue skv : d[QStringLiteral("sketches")].toArray()) {
        const QJsonObject sk = skv.toObject();
        ctx.info(QStringLiteral("sketch %1 '%2': origin=%3 normal=%4 entities=%5")
                     .arg(sk[QStringLiteral("id")].toInteger())
                     .arg(sk[QStringLiteral("name")].toString(),
                          triple(sk[QStringLiteral("origin")].toArray()),
                          triple(sk[QStringLiteral("normal")].toArray()))
                     .arg(sk[QStringLiteral("entityCount")].toInteger()));
    }

    for (const QJsonValue lv : d[QStringLiteral("layers")].toArray()) {
        const QJsonObject l = lv.toObject();
        QString line = QStringLiteral("layer '%1': 2d=%2")
                           .arg(l[QStringLiteral("name")].toString())
                           .arg(l[QStringLiteral("count")].toInteger());
        const QJsonObject counts = l[QStringLiteral("counts")].toObject();
        for (auto it = counts.begin(); it != counts.end(); ++it)
            line += QStringLiteral(" %1=%2").arg(it.key()).arg(
                it.value().toInt());
        ctx.info(line);
    }
}

class DescribeCommand : public Command {
public:
    const char* name() const override { return "DESCRIBE"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::EntitySet,
                          QStringLiteral("Select solid (id) <whole document>:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        EntityId only = kInvalidEntityId;
        if (v.kind == InputValue::Kind::EntitySet && !v.entitySet.empty()) {
            if (v.entitySet.size() != 1) {
                ctx.info(QStringLiteral("describe one solid id, or none for "
                                        "the whole document"));
                return Step::cancelled();
            }
            only = v.entitySet[0];
        } else if (v.kind == InputValue::Kind::EntityRef) {
            only = v.entityRef;
        } else if (v.kind != InputValue::Kind::Finish) {
            return Step::cancelled();
        }
        if (only != kInvalidEntityId &&
            !dynamic_cast<const SolidEntity*>(ctx.doc().entity(only))) {
            ctx.info(QStringLiteral("that id is not a solid"));
            return Step::cancelled();
        }
        describe(ctx, only);
        return Step::done();
    }
};

std::unique_ptr<Command> makeDescribe()
{
    return std::make_unique<DescribeCommand>();
}

} // namespace

void registerDescribeCommands(CommandProcessor& p)
{
    p.registerCommand(&makeDescribe, {QStringLiteral("DESC")});
}

} // namespace viki
