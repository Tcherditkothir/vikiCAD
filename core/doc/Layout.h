#pragma once

#include <QString>
#include <vector>

#include "geom/Vec2d.h"

namespace viki {

// A paper-space viewport: a window on the paper (mm, origin bottom-left)
// showing model space around `center` at `scale` (paper mm per model mm).
struct Viewport {
    double x = 10, y = 10, w = 277, h = 190; // paper rect, mm
    Vec2d center;                            // model-space center
    double scale = 1.0;                      // 1.0 = full size, 0.5 = 1:2
};

struct Layout {
    int64_t id = 0;
    QString name;
    double paperW = 297, paperH = 210; // A4 landscape default, mm
    std::vector<Viewport> viewports;
};

} // namespace viki
