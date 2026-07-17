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

// Typed object-snap candidate emitted by entities.
enum class SnapKind {
    Endpoint,
    Node,          // a POINT entity
    Intersection,
    Midpoint,
    Center,
    Quadrant,
    Perpendicular,
    Tangent,       // tangent from the rubber-band base to a circle/arc
    Nearest,       // closest point on an entity to the cursor
    Grid,
};

struct SnapPoint {
    Vec2d p;
    SnapKind kind;
};

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
    // True for unbounded entities (xline): excluded from extents, and their
    // bounds() is a huge sentinel box.
    virtual bool isInfinite() const { return false; }
    virtual void transform(const Xform2d& xf) = 0;
    virtual void buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const = 0;

    // Emits the entity's static snap candidates (endpoint/midpoint/center/
    // quadrant). Perpendicular and intersection are computed by SnapEngine.
    virtual void snapPoints(std::vector<SnapPoint>& out) const { (void)out; }

    // STRETCH: vertices inside `window` move by `delta`; the default moves
    // the whole entity when it is entirely inside, else does nothing.
    virtual void stretch(const BBox2d& window, const Vec2d& delta)
    {
        if (window.contains(bounds()))
            transform(Xform2d::translation(delta));
    }

    // Grip points for direct editing; moveGrip relocates one of them.
    virtual std::vector<Vec2d> gripPoints() const { return {}; }
    virtual void moveGrip(int index, const Vec2d& to) { (void)index; (void)to; }

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

    // Open extension bag: extra top-level JSON keys carried verbatim through
    // toJson/fromJson (and therefore .vkd, undo snapshots and queries).
    // Used by format importers for flags VikiCAD itself does not interpret
    // yet — e.g. Gerber clear polarity ("gpol":"C") or X2 attributes.
    // Reserved keys (type/layer/color/geom) cannot be overridden.
    const QJsonObject& extra() const { return m_extra; }
    void setExtra(const QJsonObject& e) { m_extra = e; }
    void setExtraValue(const QString& key, const QJsonValue& v) { m_extra[key] = v; }

protected:
    virtual void geomToJson(QJsonObject& obj) const = 0;
    virtual void geomFromJson(const QJsonObject& obj) = 0;

private:
    friend class Document;
    EntityId m_id = kInvalidEntityId;
    int64_t m_layerId = 0;
    ColorSpec m_color;
    QJsonObject m_extra;
};

// Helpers shared by entity serializers.
QJsonArray pointToJson(const Vec2d& p);
Vec2d pointFromJson(const QJsonValue& v);

} // namespace viki
