#include "Block.h"

#include <QJsonArray>

#include "Document.h"

namespace viki {

// ---- AttDefEntity -------------------------------------------------------------

void AttDefEntity::geomToJson(QJsonObject& obj) const
{
    obj[QStringLiteral("pos")] = pointToJson(m_pos);
    obj[QStringLiteral("height")] = m_height;
    obj[QStringLiteral("tag")] = m_tag;
    obj[QStringLiteral("default")] = m_default;
}

void AttDefEntity::geomFromJson(const QJsonObject& obj)
{
    m_pos = pointFromJson(obj[QStringLiteral("pos")]);
    m_height = obj[QStringLiteral("height")].toDouble(3.5);
    m_tag = obj[QStringLiteral("tag")].toString();
    m_default = obj[QStringLiteral("default")].toString();
}

// ---- InsertEntity -------------------------------------------------------------

std::unique_ptr<Entity> InsertEntity::clone() const
{
    return std::make_unique<InsertEntity>(*this);
}

BBox2d InsertEntity::bounds() const
{
    // Without the document we cannot see the definition; keep a placeholder
    // box around the insertion point. Document::entityBounds() gives the
    // real one where it matters (extents, culling, hit prefilter).
    return {position - Vec2d{1, 1}, position + Vec2d{1, 1}};
}

void InsertEntity::transform(const Xform2d& xf)
{
    position = xf.apply(position);
    const double s = xf.uniformScale();
    scale *= s;
    if (scaleY != 0.0)
        scaleY *= s;
    const double rot = std::atan2(xf.b, xf.a);
    if (xf.det() >= 0) {
        rotation = rotation + rot;
    } else {
        // Mirror: flip the Y scale and reflect the rotation.
        scaleY = -effScaleY();
        rotation = rot - rotation;
    }
}

void InsertEntity::buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const
{
    if (!ctx.doc)
        return;
    const BlockDef* def = ctx.doc->blockByName(blockName);
    if (!def) {
        // Broken reference: draw a small marker.
        StrokePrimitive s;
        s.rgb = ctx.resolvedColor;
        s.points = {position - Vec2d{2, 2}, position + Vec2d{2, 2}};
        out.strokes.push_back(std::move(s));
        return;
    }
    const Xform2d xf = insertXform(def->basePoint);
    RenderContext sub = ctx;
    for (const auto& e : def->entities) {
        if (const auto* attdef = dynamic_cast<const AttDefEntity*>(e.get())) {
            // Render the VALUE at the attdef position.
            const QString value =
                attributes.contains(attdef->tag())
                    ? attributes[attdef->tag()].toString()
                    : attdef->defaultValue();
            if (value.isEmpty())
                continue;
            TextPrimitive t;
            t.pos = xf.apply(attdef->position());
            t.height = attdef->height() * std::fabs(scale);
            t.rotation = rotation;
            t.text = value;
            t.rgb = ctx.resolvedColor;
            out.texts.push_back(std::move(t));
            continue;
        }
        auto ghost = e->clone();
        ghost->transform(xf);
        // ByLayer colors resolve against the sub-entity's own layer.
        sub.resolvedColor = ctx.doc->resolveColor(*ghost);
        ghost->buildPrimitives(sub, out);
    }
}

void InsertEntity::snapPoints(std::vector<SnapPoint>& out) const
{
    out.push_back({position, SnapKind::Endpoint});
}

void InsertEntity::geomToJson(QJsonObject& obj) const
{
    obj[QStringLiteral("block")] = blockName;
    obj[QStringLiteral("pos")] = pointToJson(position);
    obj[QStringLiteral("rotation")] = rotation;
    obj[QStringLiteral("scale")] = scale;
    if (scaleY != 0.0)
        obj[QStringLiteral("scale_y")] = scaleY;
    if (!attributes.isEmpty())
        obj[QStringLiteral("attrs")] = attributes;
}

void InsertEntity::geomFromJson(const QJsonObject& obj)
{
    blockName = obj[QStringLiteral("block")].toString();
    position = pointFromJson(obj[QStringLiteral("pos")]);
    rotation = obj[QStringLiteral("rotation")].toDouble(0.0);
    scale = obj[QStringLiteral("scale")].toDouble(1.0);
    scaleY = obj[QStringLiteral("scale_y")].toDouble(0.0);
    attributes = obj[QStringLiteral("attrs")].toObject();
}

} // namespace viki
