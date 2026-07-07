#include "QueryJson.h"

#include "doc/StickyNote.h"

namespace viki {
namespace queryjson {

QJsonObject entityJson(const Document& doc, EntityId id)
{
    const Entity* e = doc.entity(id);
    QJsonObject obj = e->toJson();
    obj[QStringLiteral("id")] = qint64(id);
    const BBox2d b = doc.entityBounds(*e);
    if (b.isValid())
        obj[QStringLiteral("bounds")] = QJsonArray{b.min.x, b.min.y, b.max.x, b.max.y};
    return obj;
}

QJsonArray entitiesJson(const Document& doc)
{
    QJsonArray out;
    for (const EntityId id : doc.drawOrder())
        out.append(entityJson(doc, id));
    return out;
}

QJsonArray layersJson(const Document& doc)
{
    QJsonArray out;
    for (const Layer& l : doc.layers())
        out.append(QJsonObject{
            {QStringLiteral("id"), qint64(l.id)},
            {QStringLiteral("name"), l.name},
            {QStringLiteral("color"),
             QStringLiteral("#%1").arg(l.rgb, 6, 16, QLatin1Char('0'))},
            {QStringLiteral("visible"), l.visible},
            {QStringLiteral("locked"), l.locked},
            {QStringLiteral("printable"), l.printable},
            {QStringLiteral("current"), l.id == doc.currentLayer()}});
    return out;
}

QJsonValue boundsJson(const Document& doc)
{
    const BBox2d b = doc.extents();
    return b.isValid() ? QJsonValue(QJsonArray{b.min.x, b.min.y, b.max.x, b.max.y})
                       : QJsonValue(QJsonArray{});
}

QJsonArray notesJson(const Document& doc)
{
    QJsonArray out;
    for (const EntityId id : doc.drawOrder()) {
        const auto* note = dynamic_cast<const StickyNoteEntity*>(doc.entity(id));
        if (!note)
            continue;
        QJsonObject obj{{QStringLiteral("id"), qint64(id)},
                        {QStringLiteral("text"), note->text},
                        {QStringLiteral("author"), note->author},
                        {QStringLiteral("created"), note->created},
                        {QStringLiteral("modified"), note->modified},
                        {QStringLiteral("anchor"),
                         QJsonArray{note->anchor.x, note->anchor.y}}};
        if (note->target != kInvalidEntityId)
            obj[QStringLiteral("target")] = qint64(note->target);
        out.append(obj);
    }
    return out;
}

QJsonArray blocksJson(const Document& doc)
{
    QJsonArray out;
    for (const auto& b : doc.blocks())
        out.append(QJsonObject{
            {QStringLiteral("id"), qint64(b->id)},
            {QStringLiteral("name"), b->name},
            {QStringLiteral("base"), QJsonArray{b->basePoint.x, b->basePoint.y}},
            {QStringLiteral("entityCount"), qint64(b->entities.size())}});
    return out;
}

QJsonArray layoutsJson(const Document& doc)
{
    QJsonArray out;
    for (const Layout& l : doc.layouts()) {
        QJsonArray vps;
        for (const Viewport& vp : l.viewports)
            vps.append(QJsonObject{{QStringLiteral("rect"),
                                    QJsonArray{vp.x, vp.y, vp.w, vp.h}},
                                   {QStringLiteral("center"),
                                    QJsonArray{vp.center.x, vp.center.y}},
                                   {QStringLiteral("scale"), vp.scale}});
        out.append(QJsonObject{{QStringLiteral("name"), l.name},
                               {QStringLiteral("paper"),
                                QJsonArray{l.paperW, l.paperH}},
                               {QStringLiteral("viewports"), vps}});
    }
    return out;
}

} // namespace queryjson
} // namespace viki
