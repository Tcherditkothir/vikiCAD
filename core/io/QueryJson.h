#pragma once

#include <QJsonArray>
#include <QJsonObject>

#include "doc/Document.h"

namespace viki {

// JSON views of the document, shared by the headless CLI and the IPC server
// so agents see one identical surface everywhere.
namespace queryjson {

QJsonObject entityJson(const Document& doc, EntityId id);
QJsonArray entitiesJson(const Document& doc);
QJsonArray layersJson(const Document& doc);
QJsonValue boundsJson(const Document& doc);
QJsonArray notesJson(const Document& doc);
QJsonArray blocksJson(const Document& doc);
QJsonArray layoutsJson(const Document& doc);

} // namespace queryjson
} // namespace viki
