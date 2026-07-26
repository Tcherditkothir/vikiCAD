#include "DwgExporter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "DxfExporter.h"

namespace viki {

QString dwgExportTool()
{
    QString tool = QStandardPaths::findExecutable(QStringLiteral("dxf2dwg"));
    if (tool.isEmpty())
        tool = QStandardPaths::findExecutable(
            QStringLiteral("dxf2dwg"),
            {QDir::homePath() + QStringLiteral("/.local/bin")});
    return tool;
}

DwgExportResult exportDwg(const Document& doc, const QString& path)
{
    DwgExportResult result;
    result.tool = dwgExportTool();
    if (result.tool.isEmpty()) {
        result.error = QStringLiteral(
            "DWG export needs dxf2dwg (GNU LibreDWG) on PATH or in "
            "~/.local/bin — install libredwg");
        return result;
    }

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        result.error = QStringLiteral("cannot create a temporary directory");
        return result;
    }

    // r2000: the version LibreDWG both reads and writes best, and the one
    // the owner-handle patch (libdxfrw patch 0005) was proven against.
    const QString dxfPath = tmp.filePath(QStringLiteral("export.dxf"));
    const DxfExportResult dxf =
        exportDxf(doc, dxfPath, QStringLiteral("2000"));
    if (!dxf.ok) {
        result.error = dxf.error;
        return result;
    }
    result.exported = dxf.exported;
    result.skipped = dxf.skipped;
    result.skippedTypes = dxf.skippedTypes;

    // Convert into the temp dir, then copy over the target only on success —
    // dxf2dwg can exit 0 after a READ ERROR without writing anything, so the
    // produced file (not the exit code) is the judge.
    const QString dwgPath = tmp.filePath(QStringLiteral("export.dwg"));
    QProcess proc;
    proc.start(result.tool,
               {QStringLiteral("-y"), QStringLiteral("--as"),
                QStringLiteral("r2000"), QStringLiteral("-o"), dwgPath,
                dxfPath});
    if (!proc.waitForFinished(120000)) {
        proc.kill();
        result.error = QStringLiteral("dxf2dwg timed out after 120 s");
        return result;
    }
    const QFileInfo produced(dwgPath);
    if (proc.exitCode() != 0 || !produced.exists() || produced.size() == 0) {
        result.error =
            QStringLiteral("dxf2dwg failed: %1")
                .arg(QString::fromLocal8Bit(proc.readAllStandardError())
                         .left(300)
                         .trimmed());
        return result;
    }

    if (QFile::exists(path) && !QFile::remove(path)) {
        result.error =
            QStringLiteral("cannot overwrite %1").arg(path);
        return result;
    }
    if (!QFile::copy(dwgPath, path)) {
        result.error = QStringLiteral("cannot write %1").arg(path);
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace viki
