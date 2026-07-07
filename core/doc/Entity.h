#pragma once

#include <cstdint>
#include <memory>

#include <QJsonObject>

#include "geom/BBox2d.h"
#include "geom/Xform2d.h"
#include "render/Primitives.h"

namespace viki {

using EntityId = int64_t;
inline constexpr EntityId kInvalidEntityId = 0;

// Entity color: ByLayer by default, otherwise explicit RGB.
struct ColorSpec {
    bool byLayer = true;
    uint32_t rgb = 0xFFFFFF;

    QJsonValue toJson() const;
    static ColorSpec fromJson(const QJsonValue& v);
};

class Entity {
public:
    virtual ~Entity() = default;

    // Stable type name, also the serialization tag ("line", "circle", ...).
    virtual const char* typeName() const = 0;
    virtual std::unique_ptr<Entity> clone() const = 0;
    virtual BBox2d bounds() const = 0;
    virtual void transform(const Xform2d& xf) = 0;
    virtual void buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const = 0;

    // Serializes the full state (type + common + geometry). Used by the native
    // format, CLI queries and undo deltas alike.
    QJsonObject toJson() const;
    // Restores common + geometry fields (not id — the Document owns ids).
    void fromJson(const QJsonObject& obj);

    EntityId id() const { return m_id; }
    int64_t layerId() const { return m_layerId; }
    void setLayerId(int64_t l) { m_layerId = l; }
    const ColorSpec& color() const { return m_color; }
    void setColor(const ColorSpec& c) { m_color = c; }

protected:
    virtual void geomToJson(QJsonObject& obj) const = 0;
    virtual void geomFromJson(const QJsonObject& obj) = 0;

private:
    friend class Document;
    EntityId m_id = kInvalidEntityId;
    int64_t m_layerId = 0;
    ColorSpec m_color;
};

// Helpers shared by entity serializers.
QJsonArray pointToJson(const Vec2d& p);
Vec2d pointFromJson(const QJsonValue& v);

} // namespace viki
