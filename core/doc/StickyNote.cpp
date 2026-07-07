#include "StickyNote.h"

#include <QJsonArray>

#include "Document.h"

namespace viki {

Vec2d StickyNoteEntity::effectiveAnchor(const Document* doc) const
{
    if (target != kInvalidEntityId && doc) {
        if (const Entity* e = doc->entity(target)) {
            const BBox2d b = doc->entityBounds(*e);
            if (b.isValid())
                return {b.max.x, b.max.y}; // pinned to the top-right corner
        }
    }
    return anchor;
}

BBox2d StickyNoteEntity::bounds() const
{
    // Anchor-based box; the flag itself is small. Pinned notes move with
    // their target at render time, so keep the box generous.
    const QStringList lines = text.split(QLatin1Char('\n'));
    double maxLen = 4;
    for (const QString& l : lines)
        maxLen = std::max(maxLen, double(l.size()));
    const double w = maxLen * 0.62 * kTextHeight + 4;
    const double h = lines.size() * 1.6 * kTextHeight + 4;
    return {anchor, anchor + Vec2d{w + 6, h + 6}};
}

void StickyNoteEntity::buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const
{
    const Vec2d at = effectiveAnchor(ctx.doc);
    const uint32_t noteColor = 0xE8C84A; // post-it yellow, always

    // Pin marker at the anchor.
    StrokePrimitive pin;
    pin.rgb = noteColor;
    pin.closed = true;
    pin.filled = true;
    pin.points = {at, at + Vec2d{1.5, 3.0}, at + Vec2d{-1.5, 3.0}};
    out.strokes.push_back(std::move(pin));

    // Note body offset up-right from the anchor.
    const QStringList lines = text.split(QLatin1Char('\n'));
    double maxLen = 4;
    for (const QString& l : lines)
        maxLen = std::max(maxLen, double(l.size()));
    const double w = maxLen * 0.62 * kTextHeight + 4;
    const double h = lines.size() * 1.6 * kTextHeight + 3;
    const Vec2d corner = at + Vec2d{3, 4};

    StrokePrimitive frame;
    frame.rgb = noteColor;
    frame.closed = true;
    frame.points = {corner, corner + Vec2d{w, 0}, corner + Vec2d{w, h}, corner + Vec2d{0, h}};
    out.strokes.push_back(std::move(frame));

    for (int i = 0; i < lines.size(); ++i) {
        TextPrimitive t;
        t.pos = corner + Vec2d{2, h - (i + 1) * 1.6 * kTextHeight + 0.4 * kTextHeight};
        t.height = kTextHeight;
        t.text = lines[i];
        t.rgb = noteColor;
        out.texts.push_back(std::move(t));
    }
}

void StickyNoteEntity::geomToJson(QJsonObject& obj) const
{
    obj[QStringLiteral("text")] = text;
    obj[QStringLiteral("author")] = author;
    obj[QStringLiteral("created")] = created;
    obj[QStringLiteral("modified")] = modified;
    obj[QStringLiteral("anchor")] = pointToJson(anchor);
    if (target != kInvalidEntityId)
        obj[QStringLiteral("target")] = qint64(target);
}

void StickyNoteEntity::geomFromJson(const QJsonObject& obj)
{
    text = obj[QStringLiteral("text")].toString();
    author = obj[QStringLiteral("author")].toString();
    created = obj[QStringLiteral("created")].toString();
    modified = obj[QStringLiteral("modified")].toString();
    anchor = pointFromJson(obj[QStringLiteral("anchor")]);
    target = obj[QStringLiteral("target")].toInteger(kInvalidEntityId);
}

} // namespace viki
