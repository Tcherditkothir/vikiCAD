#pragma once

#include <vector>

#include "Entity.h"

namespace viki {

// Multiline text. Metrics are approximated (0.62*height per char) so core
// stays free of font machinery; the canvas renders with a real font.
class TextEntity : public Entity {
public:
    TextEntity() = default;
    TextEntity(const Vec2d& pos, double height, double rotationRad, QString text)
        : m_pos(pos), m_height(height), m_rotation(rotationRad), m_text(std::move(text)) {}

    const char* typeName() const override { return "text"; }
    std::unique_ptr<Entity> clone() const override;
    BBox2d bounds() const override;
    void transform(const Xform2d& xf) override;
    void buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const override;
    void snapPoints(std::vector<SnapPoint>& out) const override;
    std::vector<Vec2d> gripPoints() const override { return {m_pos}; }
    void moveGrip(int, const Vec2d& to) override { m_pos = to; }

    Vec2d position() const { return m_pos; }
    double height() const { return m_height; }
    double rotation() const { return m_rotation; }
    QString text() const { return m_text; }
    void setText(const QString& t) { m_text = t; }
    TextHAlign hAlign = TextHAlign::Left;
    TextVAlign vAlign = TextVAlign::Baseline;
    double lineSpacing = kLineSpacing; // multiples of height (MTEXT code 44)

    static constexpr double kLineSpacing = 1.6; // multiples of height
    static constexpr double kCharAspect = 0.62; // approx width/height

    // Local-Y of the FIRST line's baseline relative to the anchor point,
    // given the vertical alignment. Shared with the DXF exporter.
    static double firstBaselineY(TextVAlign v, double height, double lineSpacing,
                                 int lineCount);

protected:
    void geomToJson(QJsonObject& obj) const override;
    void geomFromJson(const QJsonObject& obj) override;

private:
    Vec2d m_pos;
    double m_height = 3.5;
    double m_rotation = 0.0;
    QString m_text;
};

// Dimension: stores definition points + style name; every visual element is
// regenerated in buildPrimitives (measurement text follows units/style live).
class DimensionEntity : public Entity {
public:
    enum class Kind { Linear, Aligned, Angular, Radius, Diameter };

    DimensionEntity() = default;

    const char* typeName() const override { return "dimension"; }
    std::unique_ptr<Entity> clone() const override;
    BBox2d bounds() const override;
    void transform(const Xform2d& xf) override;
    void buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const override;
    void snapPoints(std::vector<SnapPoint>& out) const override;
    std::vector<Vec2d> gripPoints() const override { return {pos}; }
    void moveGrip(int, const Vec2d& to) override { pos = to; }

    Kind kind = Kind::Linear;
    // Linear/Aligned: a,b = def points. Angular: a = vertex, b,c = leg points.
    // Radius/Diameter: a = center, b = point on curve.
    Vec2d a, b, c;
    Vec2d pos;         // dimension line / arc / text position
    Vec2d axis{1, 0};  // Linear only: measurement axis (unit x or y)
    QString style = QStringLiteral("Standard");
    QString textOverride;
    // Multiplier applied to the style's absolute sizes (text height, arrows,
    // offsets). Follows the entity through transforms, so dimensions inside
    // scaled block inserts keep proportions instead of using raw mm sizes.
    double styleScale = 1.0;

    // The measurement value in mm (angle in radians for Angular).
    double measurement() const;

protected:
    void geomToJson(QJsonObject& obj) const override;
    void geomFromJson(const QJsonObject& obj) override;
};

// Leader: arrowed polyline + optional text at the far end.
class LeaderEntity : public Entity {
public:
    LeaderEntity() = default;

    const char* typeName() const override { return "leader"; }
    std::unique_ptr<Entity> clone() const override;
    BBox2d bounds() const override;
    void transform(const Xform2d& xf) override;
    void buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const override;
    void snapPoints(std::vector<SnapPoint>& out) const override;
    std::vector<Vec2d> gripPoints() const override { return points; }
    void moveGrip(int index, const Vec2d& to) override
    {
        if (index >= 0 && index < int(points.size()))
            points[size_t(index)] = to;
    }

    std::vector<Vec2d> points;
    QString text;
    QString style = QStringLiteral("Standard"); // arrow/text sizes
    double styleScale = 1.0; // see DimensionEntity::styleScale

protected:
    void geomToJson(QJsonObject& obj) const override;
    void geomFromJson(const QJsonObject& obj) override;
};

// Hatch: stored as flattened boundary rings + pattern parameters. Lines are
// regenerated on demand (even-odd rule over all rings).
class HatchEntity : public Entity {
public:
    HatchEntity() = default;

    const char* typeName() const override { return "hatch"; }
    std::unique_ptr<Entity> clone() const override;
    BBox2d bounds() const override;
    void transform(const Xform2d& xf) override;
    void buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const override;

    std::vector<std::vector<Vec2d>> rings; // closed loops, world mm
    QString pattern = QStringLiteral("ANSI31"); // SOLID | ANSI31 | ANSI37
    double scale = 1.0;
    double angle = 0.0; // extra rotation, radians

protected:
    void geomToJson(QJsonObject& obj) const override;
    void geomFromJson(const QJsonObject& obj) override;
};

// Shared helper: emit a filled arrowhead with tip at `tip` pointing along `dir`.
void emitArrow(const Vec2d& tip, const Vec2d& dir, double size, uint32_t rgb,
               PrimitiveList& out);

} // namespace viki
