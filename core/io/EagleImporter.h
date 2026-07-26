#pragma once

#include <memory>

#include <QString>
#include <QStringList>

#include "doc/Document.h"

namespace viki {

struct EagleImportResult {
    bool ok = false;
    QString error;
    int imported = 0;          // entities created
    int skipped = 0;           // drawing nodes of unsupported types
    QStringList skippedTypes;  // unique node names skipped
    int sheets = 0;            // schematic sheet count (0 for boards)
    QString kind;              // "board" | "schematic"
    std::unique_ptr<Document> document;
};

// True when the file is EAGLE 6+ XML (an <eagle> root is reachable).
bool isEagleXml(const QString& path);

// True when the file is the EAGLE 5-and-older binary container (0x10 magic).
bool isEagleBinary(const QString& path);

// Imports an EAGLE 6+ XML board (.brd) or schematic (.sch) into a fresh
// Document, viewing-grade: geometry is flattened (packages at element
// positions, symbols at instance positions), EAGLE layers become document
// layers with EAGLE's names/colors/visibility, copper wires and vias keep
// their real widths. Connectivity-only data (contactrefs, classes, design
// rules) is not represented. Schematic sheets are laid out side by side.
EagleImportResult importEagle(const QString& path);

} // namespace viki
