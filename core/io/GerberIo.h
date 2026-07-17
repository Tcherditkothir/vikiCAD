#pragma once

#include <map>
#include <vector>

#include <QMap>
#include <QString>
#include <QStringList>

#include "doc/Document.h"
#include "geom/Vec2d.h"

namespace viki {

// Gerber RS-274X (+ X2 attributes) reader, two stages:
//   1. parseGerber()      — faithful parse into a GerberFile (everything mm).
//   2. gerberToDocument() — converts a GerberFile into entities on one layer.
//
// Verified against Altium Designer 18 output (FSLAX25/MOIN, %AM macros with
// primitives 21+1, LPC/LPD, G36/G37 regions, G75 arcs) and the Ucamco
// RS-274X specification for everything the kits do not exercise.

// ---------------------------------------------------------------------------
// Stage 1 — parse
// ---------------------------------------------------------------------------

// Coordinate format from %FS: zero suppression mode + digit counts.
struct GerberFormat {
    bool omitLeading = true; // 'L' = leading zeros omitted; false = 'T'
    int intDigits = 2;
    int decDigits = 5;
    bool valid = false;      // an %FS...*% (or all-numeric header) was seen
};

enum class GerberUnit { Unknown, Inches, Millimeters };

// One %AM primitive. Length parameters are converted to mm at end of parse;
// exposure, vertex counts and rotation (degrees CCW) keep their raw values.
// Parameter layout per code (after the leading code number is stripped):
//   1  circle       : exposure, diameter, cx, cy [, rotation]
//   20 vector line  : exposure, width, xs, ys, xe, ye, rotation
//   21 center rect  : exposure, width, height, cx, cy, rotation
//   4  outline      : exposure, n, x0, y0, ..., xn, yn, rotation (n+1 points)
//   5  polygon      : exposure, n, cx, cy, diameter, rotation
// Rotation is around the MACRO origin (0,0), not the primitive center.
struct GerberMacroPrim {
    int code = 0;
    std::vector<double> params;
    int line = 0; // 1-based source line (diagnostics)
};

struct GerberMacro {
    QString name;
    std::vector<GerberMacroPrim> prims;
};

// Aperture definition (%ADDnn...). All lengths in mm.
struct GerberAperture {
    int dcode = 0;
    char kind = 'C';         // 'C','R','O','P', or 'M' = macro reference
    QString macroName;       // kind == 'M'
    // C: [diameter] ; R/O: [width, height] ; P: [outerDiameter, vertexCount,
    // rotationDeg] (rotation 0 when absent). Empty for macros.
    std::vector<double> params;
    double holeDiameter = 0.0; // round hole, 0 = none
};

// Region contour segment: line (default) or arc to `to`.
struct GerberContourSeg {
    Vec2d to;
    bool isArc = false;
    Vec2d center;  // arcs: absolute center, mm
    bool cw = false;
};

struct GerberContour {
    Vec2d start;
    std::vector<GerberContourSeg> segs;
};

enum class GerberObjKind { Draw, Arc, Flash, Region };

// One graphical object, in document order. `dark` is the polarity in force
// when the object was emitted (LPD = true, LPC = false).
struct GerberObject {
    GerberObjKind kind = GerberObjKind::Draw;
    int dcode = 0;           // selected aperture (Draw/Arc/Flash)
    bool dark = true;
    Vec2d from, to;          // Draw/Arc endpoints; Flash: `to` is the position
    Vec2d center;            // Arc: absolute center, mm
    bool cw = false;         // Arc: true = G02 (clockwise)
    bool fullCircle = false; // Arc with from == to under G75 (360 degrees)
    std::vector<GerberContour> contours; // Region
    int line = 0;            // 1-based source line
};

struct GerberFile {
    GerberFormat format;
    GerberUnit unit = GerberUnit::Unknown;
    std::map<int, GerberAperture> apertures;  // by D-code
    std::map<QString, GerberMacro> macros;    // by %AM name
    std::vector<GerberObject> objects;        // document (paint) order
    // X2 file attributes from BOTH forms — naked %TF...*% and the Altium
    // comment form `G04 #@! TF...*`. Key ".FileFunction" etc -> raw value.
    QMap<QString, QString> fileAttributes;
    // TA/TO/TD statements accepted but not interpreted (absent from the kits).
    QStringList otherAttributes;
    QStringList warnings;
    bool sawM02 = false;
};

struct GerberParseResult {
    bool ok = false;
    QString error; // "line N: ..." on parse failure
    GerberFile file;
};

GerberParseResult parseGerber(const QString& path);
GerberParseResult parseGerberData(const QByteArray& data);

// ---------------------------------------------------------------------------
// Stage 2 — conversion to entities
// ---------------------------------------------------------------------------

struct GerberImportResult {
    bool ok = false;
    QString error;
    int draws = 0;    // Gerber ops converted, by kind
    int arcs = 0;
    int flashes = 0;
    int regions = 0;
    int entities = 0; // entities added to the target layer
    int blocks = 0;   // flash aperture block definitions created
    QStringList warnings;
};

// Builds entities from `file` on the layer `layerName` (created if missing;
// must be a single token — the command grammar splits on whitespace):
//  - round-aperture draws/arcs -> PolylineEntity with stroke width (round
//    caps/joins = exact aperture footprint); consecutive continuous strokes
//    of the same aperture+polarity coalesce into one polyline,
//  - flashes -> one BlockDef per aperture (solid HatchEntity contours, name
//    "GBR-Dnn") + an InsertEntity per flash,
//  - G36/G37 regions -> solid HatchEntity (arcs tessellated at 0.001 mm),
//  - LPC objects carry "gpol":"C" in their entity JSON; document order is
//    preserved so a later renderer can erase-paint them correctly.
// All mutations run inside one transaction (single undo step).
GerberImportResult gerberToDocument(Document& doc, const GerberFile& file,
                                    const QString& layerName);

} // namespace viki
