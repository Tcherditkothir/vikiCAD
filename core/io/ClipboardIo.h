#pragma once

#include <memory>
#include <vector>

#include <QByteArray>
#include <QString>

#include "doc/Document.h"

namespace viki {

// Self-contained clipboard payload for COPYCLIP/PASTECLIP. One JSON document
// carries the entities plus everything they reference by id or name — layers,
// block definitions, dimension styles — so a paste works in the same
// document, in another document, or in another VikiCAD process. Entities use
// the same toJson/entityFromJson serialization as .vkd and the undo journal,
// so 3D solids travel with their BREP embedded.

// MIME type identifying VikiCAD payloads on the system clipboard.
inline constexpr char kClipboardMimeType[] = "application/x-vikicad-entities";

// Serializes `ids` (in document draw order) with their referenced layers,
// block definitions and dimension styles. Empty result when no id resolves.
QByteArray encodeClipboard(const Document& doc, const std::vector<EntityId>& ids);

struct ClipboardInfo {
    bool valid = false; // false: foreign or corrupt data
    int entities = 0;
    Vec2d base; // paste anchor: lower-left of the copied set's bounds
};

// Cheap header validation — no document needed, nothing mutated.
ClipboardInfo inspectClipboard(const QByteArray& payload);

// The payload's entities materialized for ghost previews: no ids, source
// layer ids left as-is. Empty on invalid payload.
std::vector<std::unique_ptr<Entity>> materializeClipboard(const QByteArray& payload);

struct PasteStats {
    int entities = 0;
    int skipped = 0; // unknown entity types (payload from a newer VikiCAD)
    int layersCreated = 0;
    int blocksCreated = 0;
    int stylesCreated = 0;
    std::vector<EntityId> ids; // the new entities, in payload order
};

// Adds the payload's entities to `doc`, translated by `offset`. Layers are
// remapped BY NAME: missing ones are created with the carried properties; an
// existing layer of the same name wins untouched (its colours/state are the
// target document's business). Same rule for block definitions and dimension
// styles. Requires an OPEN transaction for the entity adds; layer/block/
// style creation is direct (not journaled), like the LAYER and BLOCK
// commands. Returns false with `error` set on invalid payload (doc untouched).
bool pasteClipboard(Document& doc, const QByteArray& payload, const Vec2d& offset,
                    PasteStats& stats, QString& error);

// Process-local fallback transport when no system clipboard is attached to
// the CommandContext (headless CLI, tests): copy and paste still work within
// the process.
QByteArray& processClipboardBuffer();

} // namespace viki
