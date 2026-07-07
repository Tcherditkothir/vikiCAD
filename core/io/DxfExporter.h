#pragma once

#include <QString>
#include <QStringList>

#include "doc/Document.h"

namespace viki {

struct DxfExportResult {
    bool ok = false;
    QString error;
    int exported = 0;
    int skipped = 0;
    QStringList skippedTypes;
};

// Exports the document's model space to DXF. `version` accepts
// "R12", "2000", "2004", "2007", "2010", "2013" (default), "2018".
DxfExportResult exportDxf(const Document& doc, const QString& path,
                          const QString& version = QStringLiteral("2013"));

} // namespace viki
