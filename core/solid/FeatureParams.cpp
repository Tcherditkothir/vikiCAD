#include "solid/FeatureParams.h"

namespace viki {
namespace featureparams {

namespace {
const char* kindWord(FeatureKind k)
{
    switch (k) {
    case FeatureKind::Sketch:
        return "sketch";
    case FeatureKind::Extrude:
        return "extrude";
    case FeatureKind::BaseShape:
        return "base";
    case FeatureKind::Hole:
        return "hole";
    case FeatureKind::Shell:
        return "shell";
    }
    return "feature";
}

Param make(int nodeIndex, FeatureKind kind, const char* name, double value)
{
    Param p;
    p.nodeIndex = nodeIndex;
    p.name = QLatin1String(name);
    p.label = QStringLiteral("%1 %2: %3")
                  .arg(QLatin1String(kindWord(kind)))
                  .arg(nodeIndex)
                  .arg(p.name);
    p.value = value;
    return p;
}
} // namespace

std::vector<Param> list(const FeatureTree& tree)
{
    std::vector<Param> out;
    for (int i = 0; i < tree.count(); ++i) {
        const FeatureNode& n = tree.nodeAt(i);
        switch (n.kind) {
        case FeatureKind::Extrude:
            out.push_back(make(i, n.kind, "height", n.height));
            break;
        case FeatureKind::Hole:
            out.push_back(make(i, n.kind, "diameter", n.diameter));
            if (!n.through) // a through bore ignores its depth
                out.push_back(make(i, n.kind, "depth", n.depth));
            break;
        case FeatureKind::Shell:
            out.push_back(make(i, n.kind, "thickness", n.thickness));
            break;
        case FeatureKind::Sketch:    // profiles are not scalar-editable (yet)
        case FeatureKind::BaseShape: // nothing editable on a frozen brep
            break;
        }
    }
    return out;
}

bool set(FeatureTree& tree, int nodeIndex, const QString& name, double value)
{
    if (!(value > 0.0))
        return false; // every exposed param is a strictly positive length
    if (name == QLatin1String("height"))
        return tree.setExtrudeHeight(nodeIndex, value);
    if (name == QLatin1String("diameter"))
        return tree.setHoleDiameter(nodeIndex, value);
    if (name == QLatin1String("depth"))
        return tree.setHoleDepth(nodeIndex, value);
    if (name == QLatin1String("thickness"))
        return tree.setShellThickness(nodeIndex, value);
    return false;
}

} // namespace featureparams
} // namespace viki
