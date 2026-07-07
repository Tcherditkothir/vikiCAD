#pragma once

#include <memory>

#include <QJsonObject>

#include "Entity.h"

namespace viki {

// Creates an empty entity from its type name ("line", "circle", "arc"),
// nullptr for unknown types.
std::unique_ptr<Entity> createEntityByType(const QString& typeName);

// Creates and restores an entity from its full toJson() form.
std::unique_ptr<Entity> entityFromJson(const QJsonObject& obj);

} // namespace viki
