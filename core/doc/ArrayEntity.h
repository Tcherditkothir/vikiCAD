#pragma once

#include <set>
#include <vector>

#include "Entity.h"

namespace viki {

// Associative array: owns deep clones of its source entities (prototypes)
// and regenerates every item on render. Items can be suppressed; parameters
// stay editable (ARRAYEDIT) — the array is a live object, not a stamp.
class ArrayEntity : public Entity {
public:
    enum class Mode { Rectangular, Polar };

    ArrayEntity() = default;

    const char* typeName() const override { return "array"; }
    std::unique_ptr<Entity> clone() const override;
    BBox2d bounds() const override;
    void transform(const Xform2d& xf) override;
    void buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const override;
    void snapPoints(std::vector<SnapPoint>& out) const override;

    Mode mode = Mode::Rectangular;
    // Rectangular
    int rows = 1, cols = 1;
    double rowSpacing = 10.0, colSpacing = 10.0;
    // Polar
    Vec2d center;
    int count = 1;
    double angleSpan = 2.0 * M_PI; // radians, distributed CCW
    bool rotateItems = true;

    std::set<int> suppressed; // item indices skipped at regen

    // Deep-copy helpers for the prototype list (Entity is non-copyable).
    ArrayEntity(const ArrayEntity& o);
    ArrayEntity& operator=(const ArrayEntity&) = delete;

    std::vector<std::unique_ptr<Entity>> prototypes;

    int itemCount() const
    {
        return mode == Mode::Rectangular ? rows * cols : count;
    }
    Xform2d itemXform(int index) const;
    // Materializes all items (used by EXPLODE).
    std::vector<std::unique_ptr<Entity>> materialize() const;

protected:
    void geomToJson(QJsonObject& obj) const override;
    void geomFromJson(const QJsonObject& obj) override;
};

} // namespace viki
