#pragma once

#include <map>
#include <vector>

#include <QString>
#include <QStringList>

#include "doc/Document.h"
#include "geom/Vec2d.h"

namespace viki {

// Excellon (NC drill) reader, two stages:
//   1. parseExcellon()      — faithful parse into an ExcellonFile (everything mm).
//   2. excellonToDocument() — one CircleEntity per hit on a caller-named layer.
//
// Verified against Altium Designer 18 output (M48 header, `;FILE_FORMAT=2:5`
// comment, INCH,TZ, `;TYPE=PLATED`/`;TYPE=NON_PLATED` sections, TnF00S00C<dia>
// tool table, modal X/Y hits with negatives) and the Excellon documentation
// for what the kits do not exercise (LZ, METRIC, explicit decimals, G85).

// ---------------------------------------------------------------------------
// Stage 1 — parse
// ---------------------------------------------------------------------------

// Coordinate format. Excellon names the zeros that are KEPT — the opposite
// convention of Gerber %FS:
//   TZ = trailing zeros kept => LEADING zeros suppressed (value = int/10^dec),
//   LZ = leading zeros kept  => TRAILING zeros suppressed (pad right first).
struct ExcellonFormat {
    bool suppressLeading = true; // TZ; false = LZ
    bool suppressionKnown = false;
    int intDigits = 2;
    int decDigits = 4;           // INCH default 2:4; METRIC default 3:3
    bool fromComment = false;    // `;FILE_FORMAT=a:b` (Altium) forces digits
};

enum class ExcellonUnit { Unknown, Inches, Millimeters };

struct ExcellonTool {
    int number = 0;
    double diameter = 0.0; // mm (0 = declared without a C field)
    bool plated = true;    // false inside a `;TYPE=NON_PLATED` header section
    int line = 0;          // 1-based source line (diagnostics)
};

struct ExcellonHit {
    int tool = 0;
    Vec2d pos; // mm
    int line = 0;
};

// G85 slot (drilled path). Parsed and kept, with a warning — absent from the
// reference kits, never converted to entities silently.
struct ExcellonSlot {
    int tool = 0;
    Vec2d from, to; // mm
    int line = 0;
};

struct ExcellonFile {
    ExcellonFormat format;
    ExcellonUnit unit = ExcellonUnit::Unknown;
    std::map<int, ExcellonTool> tools; // by tool number
    std::vector<ExcellonHit> hits;     // file order
    std::vector<ExcellonSlot> drillSlots; // "slots" is a Qt macro keyword
    QStringList warnings;
    bool sawEnd = false; // M30 (or M00) seen
};

struct ExcellonParseResult {
    bool ok = false;
    QString error; // "line N: ..." on parse failure
    ExcellonFile file;
};

ExcellonParseResult parseExcellon(const QString& path);
ExcellonParseResult parseExcellonData(const QByteArray& data);

// ---------------------------------------------------------------------------
// Stage 2 — conversion to entities
// ---------------------------------------------------------------------------

struct ExcellonImportResult {
    bool ok = false;
    QString error;
    int hits = 0;     // drill hits converted
    int entities = 0; // entities added to the target layer
    QStringList warnings;
};

// Builds one CircleEntity per hit (radius = tool diameter / 2) on the layer
// `layerName` (created if missing; must be a single token — the command
// grammar splits on whitespace). Every entity carries "plated":true/false
// and "tool":"Tn" in its extra JSON bag, and the layer's camMeta stores the
// tool table ({"tools":{"Tn":{dia mm, plated, usage}}}) — persisted in .vkd
// for DRILLREPORT and the future Excellon exporter. Slots are NOT converted
// (warning). All mutations run inside one transaction (single undo step).
ExcellonImportResult excellonToDocument(Document& doc, const ExcellonFile& file,
                                        const QString& layerName);

} // namespace viki
