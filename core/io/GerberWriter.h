#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include "doc/Document.h"

namespace viki {

// Gerber RS-274X writer (G3). Serializes ONE document layer to a modern,
// metric, unambiguous dialect: %FSLAX46Y46*% (absolute, leading zeros
// omitted, 1e-6 mm), %MOMM*%, G75 multi-quadrant arcs, G36/G37 regions,
// %LPD*%/%LPC*% following the document paint order, M02.
//
// The aperture table is REGENERATED from the layer's entities:
//  - trace (PolylineEntity with width, "dcode" tag): the ORIGINAL aperture
//    from Layer.camMeta is reused when the width still matches it (a rect
//    aperture draw round-trips as a rect draw); an edited width becomes a
//    fresh C,<width> entry;
//  - pad (InsertEntity of a GBR-* block, "dcode" tag): the original
//    definition from camMeta is re-emitted — %AM macro bodies VERBATIM
//    (camMeta "macros" table) — with uniform insert scale/rotation folded
//    into the parameters where the standard template allows it (C: any
//    rotation; R/O: multiples of 90 deg; P: any rotation);
//  - any other transform (non-uniform scale, mirror, free rotation of a
//    rect) or a block without camMeta falls back to an outline macro
//    (primitive 4) built from the block's geometry, with a warning;
//  - region (solid HatchEntity): G36/G37, one contour per ring;
//  - definitions are deduplicated on (shape, params rounded to 1e-6 mm,
//    hole) and numbered from D10 in first-use order.
// Entities that have no Gerber image (text, dimensions, non-solid hatches,
// solids...) are skipped with a warning; the export stays usable.
struct GerberExportResult {
    bool ok = false;
    QString error;
    int entities = 0;  // entities written to the file
    int skipped = 0;   // entities with no Gerber representation (warned)
    int apertures = 0; // %AD definitions emitted
    QStringList warnings;
};

// Serializes layer `layerName` of `doc` into `out` (bytes of a complete
// RS-274X file). The document is not modified.
GerberExportResult writeGerberLayer(const Document& doc, const QString& layerName,
                                    QByteArray& out);

// Same, written to `path`.
GerberExportResult exportGerberLayer(const Document& doc, const QString& layerName,
                                     const QString& path);

} // namespace viki
