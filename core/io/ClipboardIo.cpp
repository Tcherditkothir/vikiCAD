#include "ClipboardIo.h"

#include <unordered_map>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include "doc/EntityFactory.h"

namespace viki {
namespace {

constexpr int kPayloadVersion = 1;
const QLatin1String kMagicKey("vikicad_clipboard");

// References found by walking entity JSON (no entity classes needed): the
// block a type=insert points at, the style a dimension/leader names.
void collectRefs(const QJsonObject& entity, QSet<QString>& blockNames,
                 QSet<QString>& styleNames)
{
    const QJsonObject geom = entity[QLatin1String("geom")].toObject();
    const QString block = geom[QLatin1String("block")].toString();
    if (entity[QLatin1String("type")].toString() == QLatin1String("insert") &&
        !block.isEmpty())
        blockNames.insert(block);
    const QString style = geom[QLatin1String("dimstyle")].toString();
    if (!style.isEmpty())
        styleNames.insert(style);
}

QJsonObject layerToJson(const Layer& l)
{
    QJsonObject o{{QStringLiteral("id"), qint64(l.id)},
                  {QStringLiteral("name"), l.name},
                  {QStringLiteral("color"), qint64(l.rgb)},
                  {QStringLiteral("visible"), l.visible},
                  {QStringLiteral("locked"), l.locked},
                  {QStringLiteral("printable"), l.printable},
                  {QStringLiteral("alpha"), l.alpha},
                  {QStringLiteral("rank"), l.rank},
                  {QStringLiteral("gerber_role"), l.gerberRole}};
    if (!l.camMeta.isEmpty())
        o[QStringLiteral("cam_meta")] = l.camMeta;
    return o;
}

QJsonObject parsePayload(const QByteArray& payload)
{
    if (payload.isEmpty())
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(payload);
    if (!doc.isObject())
        return {};
    const QJsonObject obj = doc.object();
    // Only version 1 exists; anything else is foreign or from the future.
    if (obj[kMagicKey].toInt(0) != kPayloadVersion)
        return {};
    return obj;
}

} // namespace

QByteArray encodeClipboard(const Document& doc, const std::vector<EntityId>& ids)
{
    const QSet<EntityId> wanted(ids.begin(), ids.end());

    // Draw order, not selection order: the paste stacks like the source.
    QJsonArray entities;
    QSet<int64_t> layerIds;
    QSet<QString> blockNames;
    QSet<QString> styleNames;
    BBox2d bounds;
    for (const EntityId id : doc.drawOrder()) {
        if (!wanted.contains(id))
            continue;
        const Entity* e = doc.entity(id);
        if (!e)
            continue;
        const QJsonObject obj = e->toJson();
        entities.push_back(obj);
        layerIds.insert(e->layerId());
        collectRefs(obj, blockNames, styleNames);
        // Infinite entities (xline) carry a sentinel box that would wreck
        // the paste anchor — same exclusion as Document::extents().
        if (!e->isInfinite())
            bounds.expand(doc.entityBounds(*e));
    }
    if (entities.isEmpty())
        return {};

    // Block definitions, including the ones nested inserts pull in; their
    // member entities add layers and styles of their own.
    QJsonArray blocks;
    QSet<QString> emitted;
    QStringList queue(blockNames.begin(), blockNames.end());
    while (!queue.isEmpty()) {
        const QString name = queue.takeFirst();
        if (emitted.contains(name))
            continue;
        emitted.insert(name);
        const BlockDef* def = doc.blockByName(name);
        if (!def)
            continue; // dangling reference: the paste side falls back too
        QJsonArray members;
        for (const auto& e : def->entities) {
            const QJsonObject obj = e->toJson();
            members.push_back(obj);
            layerIds.insert(e->layerId());
            QSet<QString> nested;
            collectRefs(obj, nested, styleNames);
            for (const QString& n : nested)
                if (!emitted.contains(n))
                    queue.push_back(n);
        }
        blocks.push_back(QJsonObject{{QStringLiteral("name"), def->name},
                                     {QStringLiteral("base"), pointToJson(def->basePoint)},
                                     {QStringLiteral("entities"), members}});
    }

    QJsonArray layers;
    for (const Layer& l : doc.layers())
        if (layerIds.contains(l.id))
            layers.push_back(layerToJson(l));

    QJsonArray styles;
    for (const DimStyle& s : doc.dimStyles())
        if (styleNames.contains(s.name))
            styles.push_back(s.toJson());

    const Vec2d base = bounds.isValid() ? bounds.min : Vec2d{0.0, 0.0};
    const QJsonObject payload{{kMagicKey, kPayloadVersion},
                              {QStringLiteral("base"), pointToJson(base)},
                              {QStringLiteral("layers"), layers},
                              {QStringLiteral("blocks"), blocks},
                              {QStringLiteral("dim_styles"), styles},
                              {QStringLiteral("entities"), entities}};
    return QJsonDocument(payload).toJson(QJsonDocument::Compact);
}

ClipboardInfo inspectClipboard(const QByteArray& payload)
{
    ClipboardInfo info;
    const QJsonObject obj = parsePayload(payload);
    if (obj.isEmpty())
        return info;
    info.valid = true;
    info.entities = obj[QLatin1String("entities")].toArray().size();
    info.base = pointFromJson(obj[QLatin1String("base")]);
    return info;
}

std::vector<std::unique_ptr<Entity>> materializeClipboard(const QByteArray& payload)
{
    std::vector<std::unique_ptr<Entity>> out;
    const QJsonObject obj = parsePayload(payload);
    for (const QJsonValue& v : obj[QLatin1String("entities")].toArray())
        if (auto e = entityFromJson(v.toObject()))
            out.push_back(std::move(e));
    return out;
}

bool pasteClipboard(Document& doc, const QByteArray& payload, const Vec2d& offset,
                    PasteStats& stats, QString& error)
{
    const QJsonObject obj = parsePayload(payload);
    if (obj.isEmpty()) {
        error = QStringLiteral("clipboard has no VikiCAD entities");
        return false;
    }

    // Layers by name; the map rewrites every entity's source layer id.
    std::unordered_map<int64_t, int64_t> layerMap;
    for (const QJsonValue& v : obj[QLatin1String("layers")].toArray()) {
        const QJsonObject lo = v.toObject();
        const int64_t oldId = lo[QLatin1String("id")].toInteger(0);
        const QString name = lo[QLatin1String("name")].toString();
        if (name.isEmpty())
            continue;
        if (Layer* existing = doc.layerByName(name)) {
            layerMap[oldId] = existing->id;
            continue;
        }
        const LayerId id =
            doc.ensureLayer(name, uint32_t(lo[QLatin1String("color")].toInteger(0xFFFFFF)),
                            lo[QLatin1String("visible")].toBool(true),
                            lo[QLatin1String("locked")].toBool(false));
        doc.setLayerPrintable(id, lo[QLatin1String("printable")].toBool(true));
        doc.setLayerAlpha(id, lo[QLatin1String("alpha")].toInt(100));
        doc.setLayerRank(id, lo[QLatin1String("rank")].toInt(0));
        doc.setLayerGerberRole(id, lo[QLatin1String("gerber_role")].toString());
        doc.setLayerCamMeta(id, lo[QLatin1String("cam_meta")].toObject());
        layerMap[oldId] = id;
        ++stats.layersCreated;
    }
    const auto remapLayer = [&](Entity& e) {
        const auto it = layerMap.find(e.layerId());
        e.setLayerId(it != layerMap.end() ? it->second : doc.currentLayer());
    };

    // Block definitions: only the missing ones; an existing definition of
    // the same name wins (standard CAD insert semantics).
    for (const QJsonValue& v : obj[QLatin1String("blocks")].toArray()) {
        const QJsonObject bo = v.toObject();
        const QString name = bo[QLatin1String("name")].toString();
        if (name.isEmpty() || doc.blockByName(name))
            continue;
        BlockDef* def =
            doc.createBlock(name, pointFromJson(bo[QLatin1String("base")]));
        for (const QJsonValue& ev : bo[QLatin1String("entities")].toArray()) {
            if (auto e = entityFromJson(ev.toObject())) {
                remapLayer(*e);
                def->entities.push_back(std::move(e));
            }
        }
        ++stats.blocksCreated;
    }

    // Dimension styles: same missing-only rule (match upsert's case rule).
    for (const QJsonValue& v : obj[QLatin1String("dim_styles")].toArray()) {
        const DimStyle style = DimStyle::fromJson(v.toObject());
        bool exists = false;
        for (const DimStyle& s : doc.dimStyles())
            exists = exists || s.name.compare(style.name, Qt::CaseInsensitive) == 0;
        if (!exists) {
            doc.upsertDimStyle(style);
            ++stats.stylesCreated;
        }
    }

    const bool translate = offset.lengthSq() > 0.0;
    for (const QJsonValue& v : obj[QLatin1String("entities")].toArray()) {
        auto e = entityFromJson(v.toObject());
        if (!e) {
            ++stats.skipped; // a type this build does not know
            continue;
        }
        remapLayer(*e);
        if (translate)
            e->transform(Xform2d::translation(offset));
        stats.ids.push_back(doc.addEntity(std::move(e)));
        ++stats.entities;
    }
    return true;
}

QByteArray& processClipboardBuffer()
{
    static QByteArray buffer;
    return buffer;
}

} // namespace viki
