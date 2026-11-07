#include "FileOpen.h"

#include <QFileInfo>

#include "doc/EntitiesEx.h"

#ifdef VIKICAD_HAS_DXF
#include "io/DxfImporter.h"
#endif
#include "io/EagleImporter.h"
#include "io/GerberKit.h"
#include "io/NativeStore.h"
#include "io/StepIo.h"
#include "io/StlIo.h"

namespace viki {

namespace {

// STEP/IGES File>Open parity: the file name tags solids only where the file
// carried no part name of its own — overwriting would throw away what XCAF
// just recovered. (Copied from MainWindow::loadFile; keep the two in step.)
void tagUnnamedSolids(Document& doc, const QString& path)
{
    const QString comp = QFileInfo(path).completeBaseName();
    for (const EntityId id : doc.drawOrder())
        if (auto* s = dynamic_cast<SolidEntity*>(doc.entity(id)))
            if (s->component.isEmpty())
                s->component = comp;
}

} // namespace

OpenFileResult openFileAny(const QString& path)
{
    OpenFileResult r;
    const QFileInfo info(path);
    const auto ends = [&path](const char* ext) {
        return path.endsWith(QLatin1String(ext), Qt::CaseInsensitive);
    };

    // A directory is a Gerber kit (fab outputs, one layer per file) — the
    // same content-sniffed importer File>Open uses.
    if (info.isDir()) {
        r.document = std::make_unique<Document>();
        const GerberKitResult k = importGerberKit(*r.document, path);
        if (!k.ok) {
            r.error = k.error;
            return r;
        }
        r.kind = QStringLiteral("gerberkit");
        r.files = int(k.files.size());
        r.entities = k.entities;
        r.layers = k.layers;
        r.skippedFiles = k.skipped;
        r.warnings = k.warnings;
        r.messages.append(
            QStringLiteral("Gerber kit: %1 file(s) -> %2 layer(s), %3 entities from %4")
                .arg(k.files.size())
                .arg(k.layers.size())
                .arg(k.entities)
                .arg(path));
        if (!k.skipped.isEmpty())
            r.messages.append(
                QStringLiteral("  skipped: %1").arg(k.skipped.join(QStringLiteral("; "))));
        if (!k.warnings.isEmpty())
            r.messages.append(
                QStringLiteral("  %1 warning(s), first: %2")
                    .arg(k.warnings.size())
                    .arg(k.warnings.first()));
        r.ok = true;
        return r;
    }

    if (ends(".vkd")) {
        QString error;
        r.document = NativeStore::load(path, error);
        if (!r.document) {
            r.error = error;
            return r;
        }
        r.kind = QStringLiteral("vkd");
        r.ok = true;
        return r;
    }

#ifdef VIKICAD_HAS_DXF
    if (ends(".dxf") || ends(".dwg")) {
        DxfImportResult imp = ends(".dwg") ? importDwg(path) : importDxf(path);
        if (!imp.ok) {
            r.error = imp.error;
            return r;
        }
        r.document = std::move(imp.document);
        r.kind = ends(".dwg") ? QStringLiteral("dwg") : QStringLiteral("dxf");
        r.imported = imp.imported;
        r.skipped = imp.skipped;
        r.skippedTypes = imp.skippedTypes;
        for (const Layer& l : r.document->layers())
            r.layers.append(l.name);
        QString msg = QStringLiteral("Imported %1 entities from %2")
                          .arg(imp.imported)
                          .arg(path);
        if (imp.imported == 0)
            msg = QStringLiteral("⚠ %1 opened but 0 entities imported (%2 skipped)")
                      .arg(path)
                      .arg(imp.skipped);
        else if (imp.skipped > 0)
            msg += QStringLiteral(" (%1 skipped: %2)")
                       .arg(imp.skipped)
                       .arg(imp.skippedTypes.join(QStringLiteral(", ")));
        r.messages.append(msg);
        r.ok = true;
        return r;
    }
#endif

    if (ends(".step") || ends(".stp")) {
        const StepResult s = importStep(path, r.document);
        if (!s.ok || !r.document) {
            r.error = s.error.isEmpty() ? QStringLiteral("STEP import failed")
                                        : s.error;
            r.document.reset();
            return r;
        }
        tagUnnamedSolids(*r.document, path);
        r.kind = QStringLiteral("step");
        r.solids = s.solids;
        r.notes = s.notes;
        r.colored = s.colored;
        r.named = s.named;
        // Say what the FILE had, so "everything is grey" is never ambiguous
        // between "no colour in the file" and "colour lost on import".
        r.messages.append(
            QStringLiteral("Imported %1 solids from %2 (%3 notes), %4")
                .arg(s.solids)
                .arg(path)
                .arg(s.notes)
                .arg(s.colored > 0 ? QStringLiteral("%1 with colour").arg(s.colored)
                                   : QStringLiteral("no colour in the file")));
        r.ok = true;
        return r;
    }

    if (ends(".igs") || ends(".iges")) {
        const StepResult s = importIges(path, r.document);
        if (!s.ok || !r.document) {
            r.error = s.error.isEmpty() ? QStringLiteral("IGES import failed")
                                        : s.error;
            r.document.reset();
            return r;
        }
        tagUnnamedSolids(*r.document, path);
        r.kind = QStringLiteral("iges");
        r.solids = s.solids;
        r.messages.append(
            QStringLiteral("Imported %1 shape(s) from IGES %2 — surface models "
                           "arrive as one viewable entity")
                .arg(s.solids)
                .arg(path));
        r.ok = true;
        return r;
    }

    if (ends(".stl")) {
        const StlResult s = importStl(path, r.document);
        if (!s.ok || !r.document) {
            r.error = s.error.isEmpty() ? QStringLiteral("STL import failed")
                                        : s.error;
            r.document.reset();
            return r;
        }
        // STL is always anonymous in its file: tag every mesh with the file
        // name, like File>Open does, so it reads in the assembly tree.
        const QString comp = QFileInfo(path).completeBaseName();
        for (const EntityId id : r.document->drawOrder())
            if (auto* se = dynamic_cast<SolidEntity*>(r.document->entity(id)))
                se->component = comp;
        r.kind = QStringLiteral("stl");
        r.solids = s.solids;
        r.triangles = s.triangles;
        r.warnings = s.warnings;
        r.messages.append(
            QStringLiteral("Imported a %1-triangle mesh from %2 — MESH2SOLID "
                           "converts it to real geometry if you need booleans")
                .arg(s.triangles)
                .arg(path));
        for (const QString& w : s.warnings)
            r.messages.append(w);
        r.ok = true;
        return r;
    }

    // EAGLE 6+ XML boards and schematics (.lbr goes through importEagle too,
    // which explains why libraries are refused). Extension first, but the
    // importer sniffs content and names the cure for v5 binaries and for
    // .sch files written by other tools.
    if (ends(".brd") || ends(".sch") || ends(".lbr")) {
        EagleImportResult imp = importEagle(path);
        if (!imp.ok) {
            r.error = imp.error;
            return r;
        }
        r.document = std::move(imp.document);
        r.kind = QStringLiteral("eagle");
        r.imported = imp.imported;
        r.skipped = imp.skipped;
        r.skippedTypes = imp.skippedTypes;
        r.sheets = imp.sheets;
        r.eagleKind = imp.kind;
        for (const Layer& l : r.document->layers())
            r.layers.append(l.name);
        QString msg = QStringLiteral("Imported %1 entities from EAGLE %2 %3")
                          .arg(imp.imported)
                          .arg(imp.kind)
                          .arg(path);
        if (imp.sheets > 1)
            msg += QStringLiteral(" (%1 sheets, side by side)").arg(imp.sheets);
        if (imp.skipped > 0)
            msg += QStringLiteral(" (%1 skipped: %2)")
                       .arg(imp.skipped)
                       .arg(imp.skippedTypes.join(QStringLiteral(", ")));
        r.messages.append(msg);
        r.ok = true;
        return r;
    }

    // A lone Gerber/Excellon file (any extension — .GTL, .TXT drills, .pho…)
    // opens as a one-file kit, exactly like the CLI import path. Content
    // sniff, so it never hijacks the known extensions handled above.
    if (looksLikeGerberOrExcellon(path)) {
        r.document = std::make_unique<Document>();
        const GerberKitResult k = importGerberKit(*r.document, path);
        if (!k.ok) {
            r.error = k.error;
            return r;
        }
        r.kind = QStringLiteral("gerberkit");
        r.files = int(k.files.size());
        r.entities = k.entities;
        r.layers = k.layers;
        r.skippedFiles = k.skipped;
        r.warnings = k.warnings;
        r.messages.append(
            QStringLiteral("Gerber kit: %1 file(s) -> %2 layer(s), %3 entities from %4")
                .arg(k.files.size())
                .arg(k.layers.size())
                .arg(k.entities)
                .arg(path));
        if (!k.skipped.isEmpty())
            r.messages.append(
                QStringLiteral("  skipped: %1").arg(k.skipped.join(QStringLiteral("; "))));
        if (!k.warnings.isEmpty())
            r.messages.append(
                QStringLiteral("  %1 warning(s), first: %2")
                    .arg(k.warnings.size())
                    .arg(k.warnings.first()));
        r.ok = true;
        return r;
    }

    // Nothing matched the dispatch — fall back to the native store. The CLI
    // `open` verb historically read .vkd content whatever the extension, and
    // that behaviour must not break behind the new routing (renamed or
    // extensionless documents). A non-vkd file fails here exactly as before.
    QString error;
    r.document = NativeStore::load(path, error);
    if (!r.document) {
        r.error = error;
        return r;
    }
    r.kind = QStringLiteral("vkd");
    r.ok = true;
    return r;
}

} // namespace viki