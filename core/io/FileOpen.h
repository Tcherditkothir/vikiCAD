#pragma once

#include <memory>

#include <QString>
#include <QStringList>

#include "doc/Document.h"
#include "solid/SolidEntity.h"

namespace viki {

// "Open any file" — the File>Open dispatch by extension, shared by the GUI
// (via its own loadFile wrapper) and the offline CLI `open` verb (issue #11:
// `open FILE.stl` used to fail with "not a VikiCAD file" while `import` and
// the GUI accepted the same file — one dispatch for every channel).
//
// Recognition order, exactly the GUI's:
//   directory              -> Gerber kit (one layer per fab file)
//   .vkd                   -> the native SQLite store
//   .dxf / .dwg            -> LibreDXF/LibreDWG importers (when built with DXF)
//   .step / .stp           -> STEP via XCAF (colours, names, notes sidecar)
//   .igs / .iges           -> IGES via the same XCAF pipeline (surfaces)
//   .stl                   -> mesh import (one mesh-backed solid)
//   .brd / .sch / .lbr     -> EAGLE 6+ XML (content-sniffed for older files)
//   lone fab file          -> Gerber/Excellon CONTENT sniff, one-file kit
//   anything else          -> native-store fallback: the historical CLI open
//                             read .vkd content whatever the extension, and
//                             that never breaks (renamed/extensionless files).
//
// Import paths tag components the way File>Open does (STEP/IGES: file name
// only where the file carried none; STL: always) so assembly trees and
// exports read the same whichever channel opened the file.

struct OpenFileResult {
    bool ok = false;
    QString error;      // set only when ok is false
    QString kind;       // "vkd" "dxf" "dwg" "step" "iges" "stl" "eagle" "gerberkit"
    QStringList messages; // File>Open parity lines (import summary, warnings)

    std::unique_ptr<Document> document; // null on failure

    // Per-kind counters, mirroring the CLI `import` JSON so a caller can read
    // the same numbers from either verb:
    int solids = 0;       // step/iges/stl
    int triangles = 0;    // stl
    int imported = 0;     // dxf/dwg/eagle
    int skipped = 0;      // dxf/dwg/eagle
    int sheets = 0;       // eagle schematics (>1 = side-by-side sheets)
    int notes = 0;        // step sidecar notes
    int colored = 0;      // step: what the FILE carried (0 = no colour in file)
    int named = 0;        // step: solids with a part name from the file
    int files = 0;        // gerberkit: files imported
    int entities = 0;     // gerberkit: entities created
    QString eagleKind;    // eagle: "board" | "schematic"
    QStringList layers;       // dxf/dwg/eagle/gerberkit
    QStringList skippedTypes; // dxf/dwg/eagle
    QStringList warnings;     // stl/gerberkit (issue #4 advisories, kit noise)
    QStringList skippedFiles; // gerberkit: "<file>: <reason>"

    // True when the file came in through an importer rather than the native
    // store. Callers use it to decide whether `--save` (which would overwrite
    // the SOURCE file with a .vkd payload) makes sense at all.
    bool importedKind() const { return !kind.isEmpty() && kind != QLatin1String("vkd"); }
};

OpenFileResult openFileAny(const QString& path);

} // namespace viki