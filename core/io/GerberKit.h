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
// leaves the GKO empty (or uses it as a real keepout ZONE) and draws the
// actual board contour on GM1 or GM13 — the candidates, in that priority
// order. A candidate is only PLAUSIBLE as the contour when it contains at
// least one stroke (a filled G36 slab is a keepout, never the outline) and
// spans most of the board along at least one axis (>= 60 % of the union of
// all Gerber layers). The plausible candidate with the best priority
// becomes "Outline"; losers keep their fallback role ("Keepout" paints
// BELOW the copper so a filled zone never masks the board). An X2
// TF.FileFunction=Profile short-circuits the election.
//
// Robustness: importing a DIRECTORY skips a file that sniffs as fab data
// but fails to parse (warning + skipped entry — one stray file must not
// make a whole board unopenable); importing an explicit SINGLE file keeps
// the parse failure as a hard error.
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

// True when `path` is a regular file whose CONTENT sniffs as Gerber RS-274X
// or Excellon (extension ignored — fab extensions lie). The GUI uses this to
// route a lone .GTL/.TXT/... through the kit importer.
bool looksLikeGerberOrExcellon(const QString& path);

} // namespace viki
