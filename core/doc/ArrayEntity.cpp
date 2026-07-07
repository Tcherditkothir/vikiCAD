#include "ArrayEntity.h"

#include <QJsonArray>

#include "EntityFactory.h"

namespace viki {

ArrayEntity::ArrayEntity(const ArrayEntity& o)
    : Entity(o), mode(o.mode), rows(o.rows), cols(o.cols), rowSpacing(o.rowSpacing),
      colSpacing(o.colSpacing), center(o.center), count(o.count), angleSpan(o.angleSpan),
      rotateItems(o.rotateItems), suppressed(o.suppressed)
{
    for (const auto& p : o.prototypes)
        prototypes.push_back(p->clone());
}

std::unique_ptr<Entity> ArrayEntity::clone() const
{
    return std::make_unique<ArrayEntity>(*this);
}

Xform2d ArrayEntity::itemXform(int index) const
{
    if (mode == Mode::Rectangular) {
        const int r = index / std::max(1, cols);
        const int col = index % std::max(1, cols);
        return Xform2d::translation({col * colSpacing, r * rowSpacing});
    }
    const bool fullCircle = nearEqual(angleSpan, 2.0 * M_PI, 1e-9);
    const int denom = fullCircle ? std::max(1, count) : std::max(1, count - 1);
    const double angle = angleSpan * index / denom;
    if (rotateItems)
        return Xform2d::rotation(angle, center);
    // Translate along the circle without rotating the item.
    const Vec2d ref = prototypes.empty() ? center : prototypes.front()->bounds().center();
    const Vec2d arm = ref - center;
    const Vec2d moved = center + arm.rotated(angle);
    return Xform2d::translation(moved - ref);
}

BBox2d ArrayEntity::bounds() const
{
    BBox2d box;
    for (int i = 0; i < itemCount(); ++i) {
        if (suppressed.count(i))
            continue;
        const Xform2d xf = itemXform(i);
        for (const auto& p : prototypes) {
            auto ghost = p->clone();
            ghost->transform(xf);
            box.expand(ghost->bounds());
        }
    }
    return box;
}

void ArrayEntity::transform(const Xform2d& xf)
{
    for (auto& p : prototypes)
        p->transform(xf);
    center = xf.apply(center);
    const double s = xf.uniformScale();
    rowSpacing *= s;
    colSpacing *= s;
    // Rectangular axes follow the rotation only in spirit (v1: axis-aligned
    // spacing scaled; full oriented arrays would store an axis pair).
}

void ArrayEntity::buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const
{
    for (int i = 0; i < itemCount(); ++i) {
        if (suppressed.count(i))
            continue;
        const Xform2d xf = itemXform(i);
        for (const auto& p : prototypes) {
            auto ghost = p->clone();
            ghost->transform(xf);
            ghost->buildPrimitives(ctx, out);
        }
    }
}

void ArrayEntity::snapPoints(std::vector<SnapPoint>& out) const
{
    // Cap the emission so a 10x10 array of polylines stays cheap.
    const int maxItems = 64;
    std::vector<SnapPoint> protoPts;
    for (const auto& p : prototypes)
        p->snapPoints(protoPts);
    for (int i = 0; i < itemCount() && i < maxItems; ++i) {
        if (suppressed.count(i))
            continue;
        const Xform2d xf = itemXform(i);
        for (const SnapPoint& sp : protoPts)
            out.push_back({xf.apply(sp.p), sp.kind});
    }
}

std::vector<std::unique_ptr<Entity>> ArrayEntity::materialize() const
{
    std::vector<std::unique_ptr<Entity>> out;
    for (int i = 0; i < itemCount(); ++i) {
        if (suppressed.count(i))
            continue;
        const Xform2d xf = itemXform(i);
        for (const auto& p : prototypes) {
            auto e = p->clone();
            e->transform(xf);
            out.push_back(std::move(e));
        }
    }
    return out;
}

void ArrayEntity::geomToJson(QJsonObject& obj) const
{
    obj[QStringLiteral("mode")] = mode == Mode::Polar ? QStringLiteral("polar")
                                                      : QStringLiteral("rect");
    obj[QStringLiteral("rows")] = rows;
    obj[QStringLiteral("cols")] = cols;
    obj[QStringLiteral("row_spacing")] = rowSpacing;
    obj[QStringLiteral("col_spacing")] = colSpacing;
    obj[QStringLiteral("center")] = pointToJson(center);
    obj[QStringLiteral("count")] = count;
    obj[QStringLiteral("angle_span")] = angleSpan;
    obj[QStringLiteral("rotate_items")] = rotateItems;
    QJsonArray sup;
    for (const int i : suppressed)
        sup.append(i);
    obj[QStringLiteral("suppressed")] = sup;
    QJsonArray protos;
    for (const auto& p : prototypes)
        protos.append(p->toJson());
    obj[QStringLiteral("prototypes")] = protos;
}

void ArrayEntity::geomFromJson(const QJsonObject& obj)
{
    mode = obj[QStringLiteral("mode")].toString() == QLatin1String("polar")
               ? Mode::Polar
               : Mode::Rectangular;
    rows = obj[QStringLiteral("rows")].toInt(1);
    cols = obj[QStringLiteral("cols")].toInt(1);
    rowSpacing = obj[QStringLiteral("row_spacing")].toDouble(10.0);
    colSpacing = obj[QStringLiteral("col_spacing")].toDouble(10.0);
    center = pointFromJson(obj[QStringLiteral("center")]);
    count = obj[QStringLiteral("count")].toInt(1);
    angleSpan = obj[QStringLiteral("angle_span")].toDouble(2.0 * M_PI);
    rotateItems = obj[QStringLiteral("rotate_items")].toBool(true);
    suppressed.clear();
    for (const QJsonValue& v : obj[QStringLiteral("suppressed")].toArray())
        suppressed.insert(v.toInt());
    prototypes.clear();
    for (const QJsonValue& v : obj[QStringLiteral("prototypes")].toArray())
        if (auto e = entityFromJson(v.toObject()))
            prototypes.push_back(std::move(e));
}

} // namespace viki
