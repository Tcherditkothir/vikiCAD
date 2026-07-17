#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include "doc/Document.h"

namespace viki {

// Excellon (NC drill) writer (G3). Serializes the drill circles of one or
// more document layers into a single metric Excellon file: M48 header,
// METRIC,TZ, ;TYPE=PLATED / ;TYPE=NON_PLATED tool sections (the Altium
// dialect our importer reads), Tn C<dia mm> table, modal hits, M30.
//
// Coordinates are EXPLICIT DECIMALS (X23.999952), not zero-suppressed
// integers. Both candidates (METRIC,TZ 3:3 integers vs decimals) rendered
// bit-identical to the original Altium kits under gerbv (dhash 0/1024,
// ink delta 0.000 pt); decimals win on robustness: they are self-describing
// — immune to the classic Excellon trap where the consumer assumes another
// digit count (3:3 vs 4:4 METRIC) or suppression mode — and they keep the
// full 1e-6 mm resolution of our geometry where 3:3 integers round at
// 1e-3 mm. METRIC,TZ is still declared so a strict consumer has a defined
// mode for any bare integer it might expect.
//
// The TOOL TABLE is regenerated from the circles: one tool per distinct
// (diameter rounded to 1e-4 mm, plated) pair; the written diameter is the
// first-encountered circle's (full 1e-6 precision, so an unedited import
// round-trips exactly). Plated tools come first, then NPTH, each group by
// ascending diameter, numbered from T1 — the shape of Altium's own table.
// Holes are written grouped by tool, in document draw order (stable);
// modality resets at every tool change (first hit restates X and Y).
struct ExcellonExportResult {
    bool ok = false;
    QString error;
    int holes = 0;   // drill hits written
    int skipped = 0; // entities with no drill image (warned)
    int tools = 0;   // Tn definitions emitted
    QStringList warnings;
};

// Serializes the circles of `layerNames` into `out` (bytes of a complete
// Excellon file). Every named layer must exist; an EMPTY list selects every
// layer whose gerberRole is Drill or Drill-NPTH (the kit importer's pair).
// A circle's plating is its "plated" tag when present (set by the Excellon
// importer); a bare CAD circle defaults to its layer's role (Drill-NPTH =>
// non-plated, anything else => plated). The document is not modified.
ExcellonExportResult writeExcellon(const Document& doc,
                                   const QStringList& layerNames, QByteArray& out);

// Same, written to `path`.
ExcellonExportResult exportExcellon(const Document& doc,
                                    const QStringList& layerNames,
                                    const QString& path);

} // namespace viki
