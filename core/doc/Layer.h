#pragma once

#include <cstdint>

#include <QJsonObject>
#include <QString>

namespace viki {

using LayerId = int64_t;

struct Layer {
    LayerId id = 0;
    QString name = QStringLiteral("0");
    uint32_t rgb = 0xFFFFFF;
    bool visible = true;
    bool locked = false;
    bool printable = true;
    // Opacity applied when the layer is composited (0 = invisible,
    // 100 = opaque). Anything below 100 routes the layer through the
    // offscreen ARGB composition path (same as LPC Gerber layers).
    int alpha = 100;
    // Paint order: layers with a LOWER rank paint first (bottom of the
    // stack). Ties keep the document draw order interleaved, so a document
    // where every layer has the default rank renders exactly as before.
    int rank = 0;
    // CAM metadata: which fabrication role this layer plays (one token of
    // gerberRoleSpecs() — "Copper-Top", "Outline"... empty = none). Set by
    // the Gerber kit importer, editable via the LAYER command / LayerPanel.
    QString gerberRole;
    // CAM source tables, set by the fab-file importers and persisted in .vkd
    // (empty for ordinary layers). Two shapes today:
    //   {"apertures": {"D10": {"shape","params"[mm],"macro"?,"hole"?,
    //                          "desc","usage"}, ...}}   (Gerber layer)
    //   {"tools":     {"T1":  {"dia"[mm],"plated","usage"}, ...}} (Excellon)
    // Consumed by the APERTURES/DRILLREPORT commands and the PropertiesPanel
    // gerber inspector; G3 export will regenerate its tables from here.
    QJsonObject camMeta;
};

} // namespace viki
