#pragma once

#include <vector>

#include <QString>

#include "Layer.h"

namespace viki {

class Document;

// The reassignable CAM role of a layer (PCB fabrication semantics), with the
// default palette the Gerber kit importer uses. Reassigning a role recolors
// the layer and moves it to the role's paint rank — the escape hatch for the
// outline-election heuristic (G1 debt): a kit whose contour was imported as
// Mech-7 becomes the magenta on-top Outline with one command / menu click.
struct GerberRoleSpec {
    QString token;   // single-token name (the command grammar splits on spaces)
    uint32_t rgb;    // palette color applied on assignment
    int rank;        // paint rank applied on assignment (lower paints first)
};

// All assignable roles, in menu order. "None" is NOT in this list: it clears
// the role and touches neither color nor rank.
const std::vector<GerberRoleSpec>& gerberRoleSpecs();

// Case-insensitive lookup; nullptr when `token` is not a role (nor "None").
const GerberRoleSpec* findGerberRole(const QString& token);

// Best-effort role token for an importer layer name ("Top-Copper" ->
// "Copper-Top", "Drill-NPTH" -> "Drill"...). Empty when nothing matches.
QString gerberRoleForLayerName(const QString& layerName);

// Assign `token` (a role or "None"/"" to clear) to the layer: sets the
// gerberRole metadata and, for a real role, applies the palette color and
// paint rank. Returns false when the layer or the role does not exist.
bool applyGerberRole(Document& doc, LayerId id, const QString& token);

} // namespace viki
