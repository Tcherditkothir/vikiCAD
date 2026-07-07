#pragma once

#include "Entity.h"

namespace viki {

// VikiCAD's signature feature: a digital post-it overlaid on the drawing.
// Non-geometric; lives on the auto-created VIKI_NOTES layer (not printable).
// Anchored to a world point, or pinned to an entity (follows its bounds).
class StickyNoteEntity : public Entity {
public:
    StickyNoteEntity() = default;

    const char* typeName() const override { return "sticky_note"; }
    std::unique_ptr<Entity> clone() const override
    {
        return std::make_unique<StickyNoteEntity>(*this);
    }
    BBox2d bounds() const override;
    void transform(const Xform2d& xf) override { anchor = xf.apply(anchor); }
    void buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const override;
    void snapPoints(std::vector<SnapPoint>& out) const override
    {
        out.push_back({anchor, SnapKind::Endpoint});
    }
    std::vector<Vec2d> gripPoints() const override { return {anchor}; }
    void moveGrip(int, const Vec2d& to) override { anchor = to; }

    QString text;         // minimal markdown (** * - [text](url)) kept verbatim
    QString author;
    QString created;      // ISO 8601
    QString modified;     // ISO 8601
    Vec2d anchor;         // world anchor (also the fallback when pin breaks)
    EntityId target = kInvalidEntityId; // pinned entity (0 = point-anchored)

    static constexpr const char* kLayerName = "VIKI_NOTES";
    static constexpr double kTextHeight = 3.0;

    // Effective anchor: pinned notes follow their target's bounds.
    Vec2d effectiveAnchor(const Document* doc) const;

protected:
    void geomToJson(QJsonObject& obj) const override;
    void geomFromJson(const QJsonObject& obj) override;
};

} // namespace viki
