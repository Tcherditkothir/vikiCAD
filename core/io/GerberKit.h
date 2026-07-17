#pragma once

#include <vector>

#include <QString>
#include <QStringList>

#include "doc/Document.h"

namespace viki {

// "Open a Gerber kit": recognizes the fabrication files of a directory (or a
// single Gerber/Excellon file), imports each one onto its own layer with a
// readable default color, and stacks them in a sensible paint order (copper
// at the bottom, outline and drill on top).
//
// Recognition is extension-based (case-insensitive, Altium/Protel names:
// GTL/GBL copper, GTS/GBS masks, GTO/GBO silkscreen, GTP/GBP paste,
// GPT/GPB pad masters, GKO keepout, GMn mechanical, TXT/DRL/... drill) but
// every candidate is content-SNIFFED first — an Excellon file must contain
// an M48 header, a Gerber file an RS-274X marker — so report files like
// Altium's "Status Report.Txt" are never mistaken for drill data. Reports
// (DRR/REP/EXTREP/RUL/LDP/apr/...) are ignored outright. When the X2
// attribute TF.FileFunction is present it PREVAILS over the extension.
//
// Outline heuristic (tolerant, verified on the reference kits): Altium often
// leaves the GKO empty and puts the real board contour on GM1. The first
// non-empty file among GKO then GM1 becomes the "Outline" layer; an empty
// candidate is skipped (files with zero graphical objects never create a
// layer), and a GM1 that lost the election stays "Mech-1".
//
// The whole kit is ONE transaction: a single undo restores the previous
// document state (layers themselves are not journaled, like every importer).

struct GerberKitFile {
    QString path;      // absolute source file
    QString layerName; // single-token target layer ("Top-Copper", "Drill"...)
    bool isDrill = false; // true = Excellon, false = Gerber RS-274X
    uint32_t rgb = 0xFFFFFF;
    int entities = 0;  // entities created on the layer
};

struct GerberKitResult {
    bool ok = false;
    QString error;
    std::vector<GerberKitFile> files; // imported files, in paint order
    QStringList layers;               // created layer names, in paint order
    QStringList skipped;              // "<file>: <reason>", not imported
    QStringList warnings;             // "<file>: <warning>"
    int entities = 0;                 // total entities created
};

GerberKitResult importGerberKit(Document& doc, const QString& path);

} // namespace viki
