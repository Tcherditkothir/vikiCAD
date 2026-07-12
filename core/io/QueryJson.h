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

// The machine half of "understand the model": a COMPUTED summary of the
// whole document with numbers as numbers and NO brep blobs anywhere.
//   units/entityCount/layerCount,
//   solids[]   id, component, volume (mm3), area (mm2), bbox{min,max} xyz,
//              centroid xyz, features[] (param-bearing nodes: extrude/hole/
//              shell, flattened param names/values — featureparams parity),
//   sketches[] id, name, plane origin/normal, entityCount,
//   layers[]   name, count + per-type counts of the 2D entities on it.
// Served by BOTH query paths ('vikicad-cli query FILE --describe' and the
// IPC 'query describe') and consumed by the DESCRIBE command for its text
// rendering, so the two views can never drift apart.
QJsonObject describeJson(const Document& doc);

} // namespace queryjson
} // namespace viki
