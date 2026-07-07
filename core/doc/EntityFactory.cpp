#include "EntityFactory.h"

#include "Entities.h"

namespace viki {

std::unique_ptr<Entity> createEntityByType(const QString& typeName)
{
    if (typeName == QLatin1String("line"))
        return std::make_unique<LineEntity>();
    if (typeName == QLatin1String("circle"))
        return std::make_unique<CircleEntity>();
    if (typeName == QLatin1String("arc"))
        return std::make_unique<ArcEntity>();
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
