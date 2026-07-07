#pragma once

#include <memory>

#include <QString>

#include "doc/Document.h"

namespace viki {

struct StepResult {
    bool ok = false;
    QString error;
    int solids = 0;
    int notes = 0; // sidecar notes written/read
};

// STEP AP214/AP242 exchange for solids. Sticky notes travel in a sidecar
// `<path>.vikinotes.json` — always written (the honest, portable Plan B;
// AP242 user-defined attributes are the M8 flag-gated Plan A).
StepResult exportStep(const Document& doc, const QString& path);
StepResult importStep(const QString& path, std::unique_ptr<Document>& outDoc);

} // namespace viki
