#include "GerberKitWriter.h"

#include <algorithm>
#include <map>

#include <QDir>
#include <QFileInfo>

#include "io/ExcellonWriter.h"
#include "io/GerberWriter.h"

namespace viki {
namespace {

bool isDrillRole(const QString& role)
{
    return role == QLatin1String("Drill") || role == QLatin1String("Drill-NPTH");
}

// Side of a fab layer, from the layer-name prefix (the same classification
// BOARDVIEW uses for sideless roles): 1 = top, -1 = bottom, 0 = unknown.
int sideOfLayerName(const QString& name)
{
    const QString n = name.toUpper();
    if (n.startsWith(QLatin1String("TOP")))
        return 1;
    if (n.startsWith(QLatin1String("BOTTOM")) || n.startsWith(QLatin1String("BOT-")))
        return -1;
    return 0;
}

// The document's layers in paint order (rank, ties keep document order) —
// the deterministic "first wins" order for extension collisions.
std::vector<const Layer*> paintOrderedLayers(const Document& doc)
{
    std::vector<const Layer*> out;
    out.reserve(doc.layers().size());
    for (const Layer& l : doc.layers())
        out.push_back(&l);
    std::stable_sort(out.begin(), out.end(),
                     [](const Layer* a, const Layer* b) { return a->rank < b->rank; });
    return out;
}

QString prefixWarnings(GerberKitExportResult& res, const QString& fileBase,
                       const QStringList& warnings)
{
    for (const QString& w : warnings)
        res.warnings << QStringLiteral("%1: %2").arg(fileBase, w);
    return {};
}

} // namespace

QString kitExtensionForLayer(const Layer& layer)
{
    const QString& role = layer.gerberRole;
    if (isDrillRole(role))
        return QStringLiteral(".TXT");
    if (role == QLatin1String("Copper-Top"))
        return QStringLiteral(".GTL");
    if (role == QLatin1String("Copper-Bottom"))
        return QStringLiteral(".GBL");
    if (role == QLatin1String("Outline"))
        return QStringLiteral(".GKO");
    const int side = sideOfLayerName(layer.name);
    if (side == 0)
        return {};
    if (role == QLatin1String("Mask"))
        return side > 0 ? QStringLiteral(".GTS") : QStringLiteral(".GBS");
    if (role == QLatin1String("Silk"))
        return side > 0 ? QStringLiteral(".GTO") : QStringLiteral(".GBO");
    if (role == QLatin1String("Paste"))
        return side > 0 ? QStringLiteral(".GTP") : QStringLiteral(".GBP");
    return {};
}

QStringList layersForKitExtension(const Document& doc, const QString& ext)
{
    QString e = ext.toUpper();
    if (e.startsWith(QLatin1Char('.')))
        e.remove(0, 1);
    QStringList out;
    for (const Layer* l : paintOrderedLayers(doc)) {
        if (e == QLatin1String("TXT") || e == QLatin1String("DRL")) {
            if (isDrillRole(l->gerberRole))
                out << l->name;
        } else if (kitExtensionForLayer(*l) == QLatin1Char('.') + e) {
            out << l->name;
        }
    }
    return out;
}

GerberKitExportResult exportGerberKit(const Document& doc, const QString& dirPath,
                                      const QString& baseName)
{
    GerberKitExportResult res;
    if (baseName.isEmpty()) {
        res.error = QStringLiteral("kit export needs a base file name");
        return res;
    }
    QDir dir(dirPath);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        res.error = QStringLiteral("cannot create directory %1").arg(dirPath);
        return res;
    }

    // Entities per layer: empty layers are skipped (Altium ships header-only
    // files; we do not re-create that noise on export).
    std::map<LayerId, int> perLayer;
    for (const EntityId id : doc.drawOrder())
        if (const Entity* e = doc.entity(id))
            ++perLayer[e->layerId()];

    QMap<QString, QString> extOwner; // ".GTS" -> layer that claimed it
    QStringList drillLayers;
    for (const Layer* l : paintOrderedLayers(doc)) {
        const int count = perLayer.count(l->id) ? perLayer[l->id] : 0;
        if (count == 0) {
            res.skippedLayers
                << QStringLiteral("%1: empty layer — nothing to write").arg(l->name);
            continue;
        }
        const QString ext = kitExtensionForLayer(*l);
        if (ext.isEmpty()) {
            res.skippedLayers << QStringLiteral(
                                     "%1: no kit extension for role '%2' — use a "
                                     "single-layer export")
                                     .arg(l->name,
                                          l->gerberRole.isEmpty()
                                              ? QStringLiteral("none")
                                              : l->gerberRole);
            continue;
        }
        if (ext == QLatin1String(".TXT")) {
            drillLayers << l->name;
            continue;
        }
        if (extOwner.contains(ext)) {
            res.skippedLayers << QStringLiteral("%1: %2 already written from '%3'")
                                     .arg(l->name, ext, extOwner[ext]);
            continue;
        }
        extOwner[ext] = l->name;
        const QString path = dir.filePath(baseName + ext);
        const GerberExportResult r = exportGerberLayer(doc, l->name, path);
        if (!r.ok) {
            res.error = QStringLiteral("%1: %2").arg(l->name, r.error);
            return res;
        }
        prefixWarnings(res, baseName + ext, r.warnings);
        res.files.push_back({QFileInfo(path).absoluteFilePath(),
                             {l->name},
                             false,
                             r.entities,
                             r.skipped});
    }

    if (!drillLayers.isEmpty()) {
        const QString path = dir.filePath(baseName + QStringLiteral(".TXT"));
        const ExcellonExportResult r = exportExcellon(doc, drillLayers, path);
        if (!r.ok) {
            res.error = QStringLiteral("%1: %2")
                            .arg(drillLayers.join(QLatin1Char('+')), r.error);
            return res;
        }
        prefixWarnings(res, baseName + QStringLiteral(".TXT"), r.warnings);
        res.files.push_back({QFileInfo(path).absoluteFilePath(), drillLayers, true,
                             r.holes, r.skipped});
    }

    if (res.files.empty()) {
        res.error = QStringLiteral(
            "no fabrication layers to export — assign CAM roles first "
            "(LAYER <name> ROLE <role>)");
        return res;
    }
    res.ok = true;
    return res;
}

GerberKitExportResult exportFabLayer(const Document& doc, const QString& layerName,
                                     const QString& path)
{
    GerberKitExportResult res;
    const Layer* layer = nullptr;
    for (const Layer& l : doc.layers())
        if (l.name.compare(layerName, Qt::CaseInsensitive) == 0)
            layer = &l;
    if (!layer) {
        res.error = QStringLiteral("no layer named '%1'").arg(layerName);
        return res;
    }
    if (isDrillRole(layer->gerberRole)) {
        const ExcellonExportResult r = exportExcellon(doc, {layer->name}, path);
        if (!r.ok) {
            res.error = r.error;
            return res;
        }
        res.warnings = r.warnings;
        res.files.push_back({QFileInfo(path).absoluteFilePath(),
                             {layer->name},
                             true,
                             r.holes,
                             r.skipped});
    } else {
        const GerberExportResult r = exportGerberLayer(doc, layer->name, path);
        if (!r.ok) {
            res.error = r.error;
            return res;
        }
        res.warnings = r.warnings;
        res.files.push_back({QFileInfo(path).absoluteFilePath(),
                             {layer->name},
                             false,
                             r.entities,
                             r.skipped});
    }
    res.ok = true;
    return res;
}

} // namespace viki
