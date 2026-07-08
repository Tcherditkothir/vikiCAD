#pragma once

#include <memory>
#include <vector>

#include "Entity.h"

namespace viki {

// Attribute definition — lives only inside a block definition. On INSERT the
// user supplies a value per tag; EXPLODE turns values into plain text.
class AttDefEntity : public Entity {
public:
    AttDefEntity() = default;
    AttDefEntity(const Vec2d& pos, double height, QString tag, QString defaultValue)
        : m_pos(pos), m_height(height), m_tag(std::move(tag)),
          m_default(std::move(defaultValue)) {}

    const char* typeName() const override { return "attdef"; }
    std::unique_ptr<Entity> clone() const override
    {
        return std::make_unique<AttDefEntity>(*this);
    }
    BBox2d bounds() const override
    {
        return {m_pos, m_pos + Vec2d{m_tag.size() * 0.62 * m_height, m_height}};
    }
    void transform(const Xform2d& xf) override
    {
        m_pos = xf.apply(m_pos);
        m_height *= xf.uniformScale();
    }
    void buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const override
    {
        // In-definition display: the tag name.
        TextPrimitive t;
        t.pos = m_pos;
        t.height = m_height;
        t.text = m_tag;
        t.rgb = ctx.resolvedColor;
        out.texts.push_back(std::move(t));
    }

    Vec2d position() const { return m_pos; }
    double height() const { return m_height; }
    QString tag() const { return m_tag; }
    QString defaultValue() const { return m_default; }

protected:
    void geomToJson(QJsonObject& obj) const override;
    void geomFromJson(const QJsonObject& obj) override;

private:
    Vec2d m_pos;
    double m_height = 3.5;
    QString m_tag;
    QString m_default;
};

struct BlockDef {
    int64_t id = 0;
    QString name;
    Vec2d basePoint;
    std::vector<std::unique_ptr<Entity>> entities;
};

// Reference to a block definition placed in model space.
class InsertEntity : public Entity {
public:
    InsertEntity() = default;

    const char* typeName() const override { return "insert"; }
    std::unique_ptr<Entity> clone() const override;
    BBox2d bounds() const override;
    void transform(const Xform2d& xf) override;
    void buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const override;
    void snapPoints(std::vector<SnapPoint>& out) const override;
    std::vector<Vec2d> gripPoints() const override { return {position}; }
    void moveGrip(int, const Vec2d& to) override { position = to; }

    QString blockName;
    Vec2d position;
    double rotation = 0.0; // radians
    double scale = 1.0;    // X scale (may be negative = mirrored)
    double scaleY = 0.0;   // Y scale; 0 = same as scale (uniform)
    QJsonObject attributes; // tag -> value

    double effScaleY() const { return scaleY == 0.0 ? scale : scaleY; }

    Xform2d insertXform(const Vec2d& basePoint) const
    {
        // T(position) * R * S(sx, sy) * T(-base)
        Xform2d s;
        s.a = scale;
        s.d = effScaleY();
        return Xform2d::translation(position)
            .compose(Xform2d::rotation(rotation))
            .compose(s)
            .compose(Xform2d::translation(basePoint * -1.0));
    }

protected:
    void geomToJson(QJsonObject& obj) const override;
    void geomFromJson(const QJsonObject& obj) override;
};

} // namespace viki
