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

// Imports a DWG file through libdxfrw's dwg reader (R14..2015 coverage,
// LibreCAD's reader). Same conversion pipeline as DXF.
DxfImportResult importDwg(const QString& path);

// Decodes MTEXT inline formatting to plain text: \P -> newline, group braces
// and {\f...;}/\H/\W/\C/... runs stripped, \S stacks -> "a/b", \U+XXXX and
// %%-symbols resolved. Exposed for tests.
QString decodeMtextContent(const QString& raw);

// Decodes TEXT %%-symbols: %%d -> degree, %%c -> diameter, %%p -> plus/minus,
// %%u/%%o style toggles dropped, %%% -> '%'.
QString decodeTextSymbols(const QString& raw);

} // namespace viki
