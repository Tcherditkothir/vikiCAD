#pragma once

#include <vector>

#include <QString>
#include <QStringList>

#include "doc/Document.h"

namespace viki {

// "Export a Gerber kit" (G3): the inverse of io/GerberKit.h. Writes every
// fabrication layer of the document into a directory as <baseName>.<EXT>,
// Altium/Protel extensions keyed on the layer's CAM role (Layer.gerberRole)
// and side (Top-/Bottom- layer-name prefix, the BOARDVIEW classification):
//
//   Copper-Top .GTL | Copper-Bottom .GBL | Mask .GTS/.GBS | Silk .GTO/.GBO
//   Paste .GTP/.GBP | Outline .GKO       | Drill+Drill-NPTH -> ONE .TXT
//
// Gerber layers go through io/GerberWriter.h, the drill layers are grouped
// into a single Excellon file via io/ExcellonWriter.h (plated + NPTH
// sections, the Altium shape our importer reads back as the Drill /
// Drill-NPTH pair). Layers with no kit mapping (no role, Mech, Keepout, a
// sideless Mask...) and empty layers are listed in `skippedLayers` — export
// them individually with exportFabLayer when needed. A writer hard error
// (coordinate out of range...) aborts the whole export.
struct GerberKitExportFile {
    QString path;       // file written (absolute)
    QStringList layers; // source layer(s) — several only for the drill file
    bool isDrill = false;
    int entities = 0;   // entities written (drill file: holes)
    int skipped = 0;    // entities the writer had no image for (warned)
};

struct GerberKitExportResult {
    bool ok = false;
    QString error;
    std::vector<GerberKitExportFile> files;
    QStringList skippedLayers; // "<layer>: <reason>" — nothing written
    QStringList warnings;      // writer warnings, prefixed "<file base>: "
};

// Kit extension for one layer (".GTL", ".TXT"...). Empty when the layer has
// no kit mapping. Pure role/name lookup — does not check for entities.
QString kitExtensionForLayer(const Layer& layer);

// Layer names of `doc` that match kit extension `ext` ("gtl", ".GTS",
// case-insensitive; "TXT"/"DRL" = every Drill-role layer), in stack order.
QStringList layersForKitExtension(const Document& doc, const QString& ext);

// Export the whole kit into `dirPath` (created if missing) as
// <baseName>.<EXT>. The document is not modified.
GerberKitExportResult exportGerberKit(const Document& doc, const QString& dirPath,
                                      const QString& baseName);

// Export ONE layer to `path`, routing Drill-role layers to the Excellon
// writer and everything else to the RS-274X writer. The single entry in
// `files` carries the counts.
GerberKitExportResult exportFabLayer(const Document& doc, const QString& layerName,
                                     const QString& path);

} // namespace viki
