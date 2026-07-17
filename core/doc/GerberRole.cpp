#include "GerberRole.h"

#include "Document.h"

namespace viki {

// Palette + ranks aligned with the Gerber kit importer (GerberKit.cpp):
// copper at the bottom of the stack, outline/drill on top. The generic role
// ranks sit on the importer's scale so a reassigned layer lands in a
// sensible place among imported ones.
const std::vector<GerberRoleSpec>& gerberRoleSpecs()
{
    static const std::vector<GerberRoleSpec> kSpecs = {
        {QStringLiteral("Copper-Bottom"), 0x3D7EFF, 10},
        {QStringLiteral("Copper-Top"), 0xE53935, 20},
        {QStringLiteral("Paste"), 0xB0B7BD, 30},
        {QStringLiteral("Mask"), 0x2FBF71, 35},
        {QStringLiteral("Silk"), 0xF2D544, 40},
        {QStringLiteral("Mech"), 0x8FA3B0, 60},
        {QStringLiteral("Outline"), 0xFF00FF, 90},
        {QStringLiteral("Drill"), 0x000000, 95},
    };
    return kSpecs;
}

const GerberRoleSpec* findGerberRole(const QString& token)
{
    for (const GerberRoleSpec& s : gerberRoleSpecs())
        if (s.token.compare(token, Qt::CaseInsensitive) == 0)
            return &s;
    return nullptr;
}

QString gerberRoleForLayerName(const QString& layerName)
{
    const QString n = layerName.toUpper();
    if (n.startsWith(QLatin1String("TOP-COPPER")))
        return QStringLiteral("Copper-Top");
    if (n.startsWith(QLatin1String("BOTTOM-COPPER")))
        return QStringLiteral("Copper-Bottom");
    if (n.contains(QLatin1String("MASK")))
        return QStringLiteral("Mask");
    if (n.contains(QLatin1String("SILK")))
        return QStringLiteral("Silk");
    if (n.contains(QLatin1String("PASTE")))
        return QStringLiteral("Paste");
    if (n.startsWith(QLatin1String("OUTLINE")))
        return QStringLiteral("Outline");
    if (n.startsWith(QLatin1String("DRILL")))
        return QStringLiteral("Drill");
    if (n.startsWith(QLatin1String("MECH")))
        return QStringLiteral("Mech");
    return {};
}

bool applyGerberRole(Document& doc, LayerId id, const QString& token)
{
    if (!doc.layer(id))
        return false;
    if (token.isEmpty() || token.compare(QLatin1String("None"), Qt::CaseInsensitive) == 0) {
        doc.setLayerGerberRole(id, QString());
        return true;
    }
    const GerberRoleSpec* spec = findGerberRole(token);
    if (!spec)
        return false;
    const Layer* l = doc.layer(id);
    doc.setLayerGerberRole(id, spec->token);
    doc.setLayerProps(id, spec->rgb, l->visible, l->locked);
    doc.setLayerRank(id, spec->rank);
    return true;
}

} // namespace viki
