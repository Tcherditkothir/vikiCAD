#pragma once

#include <vector>

#include "Entity.h"

namespace viki {

// Polyline vertex: bulge = tan(sweep/4) of the arc segment to the NEXT
// vertex (0 = straight, >0 CCW, <0 CW) — DXF-compatible convention.
struct PolyVertex {
    Vec2d pos;
    double bulge = 0.0;
};

class PolylineEntity : public Entity {
public:
    PolylineEntity() = default;
    PolylineEntity(std::vector<PolyVertex> vertices, bool closed)
        : m_vertices(std::move(vertices)), m_closed(closed) {}

    const char* typeName() const override { return "polyline"; }
    std::unique_ptr<Entity> clone() const override;
    BBox2d bounds() const override;
    void transform(const Xform2d& xf) override;
    void buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const override;
    void snapPoints(std::vector<SnapPoint>& out) const override;
    void stretch(const BBox2d& window, const Vec2d& delta) override;
    std::vector<Vec2d> gripPoints() const override;
    void moveGrip(int index, const Vec2d& to) override;

    const std::vector<PolyVertex>& vertices() const { return m_vertices; }
    bool isClosed() const { return m_closed; }

protected:
    void geomToJson(QJsonObject& obj) const override;
    void geomFromJson(const QJsonObject& obj) override;

private:
    std::vector<PolyVertex> m_vertices;
    bool m_closed = false;
};

// Ellipse (or elliptical arc when [startParam, endParam) != full turn).
// majorAxis = semi-major axis vector from center; ratio = minor/major.
// Parametrization: P(t) = center + major*cos(t) + minor*sin(t).
class EllipseEntity : public Entity {
public:
    EllipseEntity() = default;
    EllipseEntity(const Vec2d& center, const Vec2d& majorAxis, double ratio,
                  double startParam = 0.0, double endParam = 2.0 * M_PI)
        : m_center(center), m_major(majorAxis), m_ratio(ratio),
          m_startParam(startParam), m_endParam(endParam) {}

    const char* typeName() const override { return "ellipse"; }
    std::unique_ptr<Entity> clone() const override;
    BBox2d bounds() const override;
    void transform(const Xform2d& xf) override;
    void buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const override;
    void snapPoints(std::vector<SnapPoint>& out) const override;

    Vec2d center() const { return m_center; }
    Vec2d majorAxis() const { return m_major; }
    double ratio() const { return m_ratio; }
    bool isFull() const { return nearEqual(m_endParam - m_startParam, 2.0 * M_PI, 1e-9); }
    double startParam() const { return m_startParam; }
    double endParam() const { return m_endParam; }
    Vec2d pointAt(double t) const
    {
        return m_center + m_major * std::cos(t) + m_major.perp() * (m_ratio * std::sin(t));
    }

protected:
    void geomToJson(QJsonObject& obj) const override;
    void geomFromJson(const QJsonObject& obj) override;

private:
    Vec2d m_center;
    Vec2d m_major{1, 0};
    double m_ratio = 0.5;
    double m_startParam = 0.0;
    double m_endParam = 2.0 * M_PI;
};

// NURBS curve. With control points: degree/knots/weights honored (de Boor).
// With only fit points (DXF splines sometimes): rendered as the fit polyline
// chord — honest degradation until proper interpolation lands.
class SplineEntity : public Entity {
public:
    SplineEntity() = default;

    const char* typeName() const override { return "spline"; }
    std::unique_ptr<Entity> clone() const override;
    BBox2d bounds() const override;
    void transform(const Xform2d& xf) override;
    void buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const override;
    void snapPoints(std::vector<SnapPoint>& out) const override;
    void stretch(const BBox2d& window, const Vec2d& delta) override;

    int degree = 3;
    std::vector<Vec2d> controlPoints;
    std::vector<double> knots;
    std::vector<double> weights; // empty = non-rational
    std::vector<Vec2d> fitPoints;
    bool closed = false;

    // Evaluate at parameter u (requires control points + knots).
    Vec2d evaluate(double u) const;

protected:
    void geomToJson(QJsonObject& obj) const override;
    void geomFromJson(const QJsonObject& obj) override;
};

class PointEntity : public Entity {
public:
    PointEntity() = default;
    explicit PointEntity(const Vec2d& pos) : m_pos(pos) {}

    const char* typeName() const override { return "point"; }
    std::unique_ptr<Entity> clone() const override;
    BBox2d bounds() const override { return {m_pos, m_pos}; }
    void transform(const Xform2d& xf) override { m_pos = xf.apply(m_pos); }
    void buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const override;
    void snapPoints(std::vector<SnapPoint>& out) const override;

    Vec2d position() const { return m_pos; }

protected:
    void geomToJson(QJsonObject& obj) const override;
    void geomFromJson(const QJsonObject& obj) override;

private:
    Vec2d m_pos;
};

// Infinite construction line through basePoint along direction.
class XLineEntity : public Entity {
public:
    XLineEntity() = default;
    XLineEntity(const Vec2d& base, const Vec2d& direction)
        : m_base(base), m_dir(direction.normalized()) {}

    const char* typeName() const override { return "xline"; }
    std::unique_ptr<Entity> clone() const override;
    bool isInfinite() const override { return true; }
    BBox2d bounds() const override
    {
        return {{-1e9, -1e9}, {1e9, 1e9}};
    }
    void transform(const Xform2d& xf) override
    {
        m_base = xf.apply(m_base);
        m_dir = xf.applyVector(m_dir).normalized();
    }
    void buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const override;

    Vec2d basePoint() const { return m_base; }
    Vec2d direction() const { return m_dir; }

protected:
    void geomToJson(QJsonObject& obj) const override;
    void geomFromJson(const QJsonObject& obj) override;

private:
    Vec2d m_base;
    Vec2d m_dir{1, 0};
};

} // namespace viki
