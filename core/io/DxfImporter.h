#pragma once

#include <memory>

#include <QString>
#include <QStringList>

#include "doc/Document.h"

namespace viki {

struct DxfImportResult {
    bool ok = false;
    QString error;
    int imported = 0;                 // entities converted
    int skipped = 0;                  // entities of unsupported types
    QStringList skippedTypes;         // unique type names skipped
    std::unique_ptr<Document> document;
};

// Imports a DXF file (R12..2018, ASCII or binary) into a fresh Document.
// Model space only; unsupported entity types are counted, not fatal.
DxfImportResult importDxf(const QString& path);

} // namespace viki
