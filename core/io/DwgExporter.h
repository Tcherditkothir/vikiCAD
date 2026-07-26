#pragma once

#include <QString>
#include <QStringList>

#include "doc/Document.h"

namespace viki {

struct DwgExportResult {
    bool ok = false;
    QString error;
    int exported = 0; // entities written to the DXF stage
    int skipped = 0;
    QStringList skippedTypes;
    QString tool; // resolved dxf2dwg executable (empty when not installed)
};

// Resolved dxf2dwg executable (PATH, then ~/.local/bin — the same lookup as
// the dwg2dxf import fallback). Empty when GNU LibreDWG is not installed;
// callers can test this before offering DWG export.
QString dwgExportTool();

// Exports the document's model space to DWG r2000: writes a temporary DXF
// (patch 0005 makes it canonical enough for LibreDWG) and converts it with
// dxf2dwg. The DWG is written to a temporary file first and only copied over
// `path` on success, so a failed conversion never leaves a truncated target.
DwgExportResult exportDwg(const Document& doc, const QString& path);

} // namespace viki
