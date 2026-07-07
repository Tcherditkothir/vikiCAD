#include "EntityFactory.h"

#include "Annotations.h"
#include "ArrayEntity.h"
#include "Block.h"
#include "StickyNote.h"
#include "Entities.h"
#include "EntitiesEx.h"

namespace viki {

std::unique_ptr<Entity> createEntityByType(const QString& typeName)
{
    if (typeName == QLatin1String("line"))
        return std::make_unique<LineEntity>();
    if (typeName == QLatin1String("circle"))
        return std::make_unique<CircleEntity>();
    if (typeName == QLatin1String("arc"))
        return std::make_unique<ArcEntity>();
    if (typeName == QLatin1String("polyline"))
        return std::make_unique<PolylineEntity>();
    if (typeName == QLatin1String("ellipse"))
        return std::make_unique<EllipseEntity>();
    if (typeName == QLatin1String("spline"))
        return std::make_unique<SplineEntity>();
    if (typeName == QLatin1String("point"))
        return std::make_unique<PointEntity>();
    if (typeName == QLatin1String("xline"))
        return std::make_unique<XLineEntity>();
    if (typeName == QLatin1String("text"))
        return std::make_unique<TextEntity>();
    if (typeName == QLatin1String("dimension"))
        return std::make_unique<DimensionEntity>();
    if (typeName == QLatin1String("leader"))
        return std::make_unique<LeaderEntity>();
    if (typeName == QLatin1String("hatch"))
        return std::make_unique<HatchEntity>();
    if (typeName == QLatin1String("insert"))
        return std::make_unique<InsertEntity>();
    if (typeName == QLatin1String("attdef"))
        return std::make_unique<AttDefEntity>();
    if (typeName == QLatin1String("array"))
        return std::make_unique<ArrayEntity>();
    if (typeName == QLatin1String("sticky_note"))
        return std::make_unique<StickyNoteEntity>();
    return nullptr;
}

std::unique_ptr<Entity> entityFromJson(const QJsonObject& obj)
{
    auto entity = createEntityByType(obj[QStringLiteral("type")].toString());
    if (entity)
        entity->fromJson(obj);
    return entity;
}

} // namespace viki
