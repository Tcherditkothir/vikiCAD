#pragma once

#include "Entity.h"

namespace viki {

class LineEntity : public Entity {
public:
    LineEntity() = default;
    LineEntity(const Vec2d& p1, const Vec2d& p2) : m_p1(p1), m_p2(p2) {}

    const char* typeName() const override { return "line"; }
    std::unique_ptr<Entity> clone() const override;
    BBox2d bounds() const override { return {m_p1, m_p2}; }
    void transform(const Xform2d& xf) override;
    void buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const override;
    void snapPoints(std::vector<SnapPoint>& out) const override;

    Vec2d p1() const { return m_p1; }
    Vec2d p2() const { return m_p2; }

protected:
    void geomToJson(QJsonObject& obj) const override;
    void geomFromJson(const QJsonObject& obj) override;

private:
    Vec2d m_p1, m_p2;
};

class CircleEntity : public Entity {
public:
    CircleEntity() = default;
    CircleEntity(const Vec2d& center, double radius) : m_center(center), m_radius(radius) {}

    const char* typeName() const override { return "circle"; }
    std::unique_ptr<Entity> clone() const override;
    BBox2d bounds() const override
    {
        return {m_center - Vec2d{m_radius, m_radius}, m_center + Vec2d{m_radius, m_radius}};
    }
    void transform(const Xform2d& xf) override;
    void buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const override;
    void snapPoints(std::vector<SnapPoint>& out) const override;

    Vec2d center() const { return m_center; }
    double radius() const { return m_radius; }

protected:
    void geomToJson(QJsonObject& obj) const override;
    void geomFromJson(const QJsonObject& obj) override;

private:
    Vec2d m_center;
    double m_radius = 1.0;
};

// Circular arc, CCW from startAngle over sweep (radians, sweep > 0).
class ArcEntity : public Entity {
public:
    ArcEntity() = default;
    ArcEntity(const Vec2d& center, double radius, double startAngle, double sweep)
        : m_center(center), m_radius(radius), m_startAngle(startAngle), m_sweep(sweep) {}

    const char* typeName() const override { return "arc"; }
    std::unique_ptr<Entity> clone() const override;
    BBox2d bounds() const override;
    void transform(const Xform2d& xf) override;
    void buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const override;
    void snapPoints(std::vector<SnapPoint>& out) const override;

    Vec2d center() const { return m_center; }
    double radius() const { return m_radius; }
    double startAngle() const { return m_startAngle; }
    double sweep() const { return m_sweep; }
    Vec2d startPoint() const { return m_center + Vec2d::polar(m_radius, m_startAngle); }
    Vec2d endPoint() const { return m_center + Vec2d::polar(m_radius, m_startAngle + m_sweep); }

protected:
    void geomToJson(QJsonObject& obj) const override;
    void geomFromJson(const QJsonObject& obj) override;

private:
    Vec2d m_center;
    double m_radius = 1.0;
    double m_startAngle = 0.0;
    double m_sweep = M_PI;
};

// Flatten a circular arc into `out.points` (appends; includes both ends).
void flattenArc(const Vec2d& center, double radius, double startAngle, double sweep,
                double chordTolerance, std::vector<Vec2d>& out);

} // namespace viki
