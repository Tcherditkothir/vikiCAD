#include "QueryJson.h"

#include <map>

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <gp_Pnt.hxx>

#include "doc/StickyNote.h"
#include "solid/SolidEntity.h"

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

namespace {

QJsonArray xyz(const gp_Pnt& p)
{
    return QJsonArray{p.X(), p.Y(), p.Z()};
}

QJsonArray xyz(const gp_Dir& d)
{
    return QJsonArray{d.X(), d.Y(), d.Z()};
}

// One JSON entry per param-bearing feature node (extrude/hole/shell —
// the same nodes featureparams::list exposes), params flattened as
// name:number pairs. Sketch/BaseShape nodes carry nothing scalar and are
// skipped, so features[0] of a box+hole solid IS the hole.
QJsonArray featuresJson(const FeatureTree& tree)
{
    QJsonArray out;
    for (int i = 0; i < tree.count(); ++i) {
        const FeatureNode& n = tree.nodeAt(i);
        QJsonObject f{{QStringLiteral("node"), i}};
        switch (n.kind) {
        case FeatureKind::Extrude:
            f[QStringLiteral("kind")] = QStringLiteral("extrude");
            f[QStringLiteral("height")] = n.height;
            break;
        case FeatureKind::Hole:
            f[QStringLiteral("kind")] = QStringLiteral("hole");
            f[QStringLiteral("diameter")] = n.diameter;
            f[QStringLiteral("through")] = n.through;
            if (!n.through) // a through bore ignores its depth
                f[QStringLiteral("depth")] = n.depth;
            f[QStringLiteral("center")] =
                QJsonArray{n.holeCenter.x, n.holeCenter.y};
            break;
        case FeatureKind::Shell:
            f[QStringLiteral("kind")] = QStringLiteral("shell");
            f[QStringLiteral("thickness")] = n.thickness;
            break;
        case FeatureKind::Sketch:
        case FeatureKind::BaseShape:
            continue; // no scalar params (featureparams parity)
        }
        out.append(f);
    }
    return out;
}

} // namespace

QJsonObject describeJson(const Document& doc)
{
    QJsonObject out;
    out[QStringLiteral("units")] =
        doc.displayUnits() == DisplayUnits::Inches ? QStringLiteral("in")
                                                   : QStringLiteral("mm");
    out[QStringLiteral("entityCount")] = qint64(doc.entityCount());
    out[QStringLiteral("layerCount")] = qint64(doc.layers().size());

    QJsonArray solids;
    for (const EntityId id : doc.drawOrder()) {
        const auto* s = dynamic_cast<const SolidEntity*>(doc.entity(id));
        if (!s)
            continue;
        QJsonObject o{{QStringLiteral("id"), qint64(id)},
                      {QStringLiteral("component"), s->component}};
        GProp_GProps vol;
        BRepGProp::VolumeProperties(s->shape(), vol);
        o[QStringLiteral("volume")] = vol.Mass(); // mm3
        GProp_GProps surf;
        BRepGProp::SurfaceProperties(s->shape(), surf);
        o[QStringLiteral("area")] = surf.Mass(); // mm2
        const BBox2d b = s->bounds();
        o[QStringLiteral("bbox")] = QJsonObject{
            {QStringLiteral("min"), QJsonArray{b.min.x, b.min.y, s->zMin()}},
            {QStringLiteral("max"), QJsonArray{b.max.x, b.max.y, s->zMax()}}};
        o[QStringLiteral("centroid")] = xyz(vol.CentreOfMass());
        o[QStringLiteral("features")] =
            s->features ? featuresJson(*s->features) : QJsonArray{};
        solids.append(o);
    }
    out[QStringLiteral("solids")] = solids;

    QJsonArray sketches;
    for (const SketchInfo& sk : doc.sketches())
        sketches.append(QJsonObject{
            {QStringLiteral("id"), qint64(sk.id)},
            {QStringLiteral("name"), sk.name},
            {QStringLiteral("origin"), xyz(sk.plane.origin)},
            {QStringLiteral("normal"), xyz(sk.plane.normal)},
            {QStringLiteral("entityCount"),
             qint64(doc.sketchEntities(sk.id).size())}});
    out[QStringLiteral("sketches")] = sketches;

    QJsonArray layers;
    for (const Layer& l : doc.layers()) {
        // Per-type counts of the 2D entities on this layer (solids live in
        // the solids[] section; counting them here twice would only confuse).
        std::map<QString, int> counts; // sorted -> deterministic output
        qint64 total = 0;
        for (const EntityId id : doc.drawOrder()) {
            const Entity* e = doc.entity(id);
            if (e->layerId() != l.id || dynamic_cast<const SolidEntity*>(e))
                continue;
            ++counts[QLatin1String(e->typeName())];
            ++total;
        }
        QJsonObject byType;
        for (const auto& [type, n] : counts)
            byType[type] = n;
        layers.append(QJsonObject{{QStringLiteral("name"), l.name},
                                  {QStringLiteral("count"), total},
                                  {QStringLiteral("counts"), byType}});
    }
    out[QStringLiteral("layers")] = layers;
    return out;
}

} // namespace queryjson
} // namespace viki
