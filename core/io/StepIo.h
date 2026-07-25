#pragma once

#include <memory>

#include <QString>

#include "doc/Document.h"

namespace viki {

struct StepResult {
    bool ok = false;
    QString error;
    int solids = 0;
    int notes = 0;    // sidecar notes written/read
    int colored = 0;  // solids that arrived with a colour from the file
    int named = 0;    // solids that arrived with a part name from the file
};

// STEP AP214/AP242 exchange for solids. Sticky notes travel in a sidecar
// `<path>.vikinotes.json` — always written (the honest, portable Plan B;
// AP242 user-defined attributes are the M8 flag-gated Plan A).
StepResult exportStep(const Document& doc, const QString& path);

// Import through XCAF, so presentation data survives alongside the geometry:
// per-solid colour (STEP's STYLED_ITEM / COLOUR_RGB) into the entity colour,
// alpha into SolidEntity::transparency, and the part name into
// SolidEntity::component so the assembly tree reads properly.
//
// Plain STEPControl_Reader — what this used to use — transfers geometry ONLY
// and silently drops all of that. Not every file carries it (a bare AP203
// export has no colour entities at all), hence the `colored` / `named` counters:
// they say what the FILE had, so "no colour" is never confused with "colour
// lost".
StepResult importStep(const QString& path, std::unique_ptr<Document>& outDoc);

} // namespace viki
