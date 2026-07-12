#pragma once

#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

namespace viki {
namespace solidops {

// The quick numbers every "understand this solid" view needs — computed in
// ONE place and reused by queryjson::describeJson (DESCRIBE / query
// describe) and the LIST command. All values are millimetre-based.
struct SolidMetrics {
    bool valid = false;      // false: null/empty shape, everything else is 0
    double volume = 0.0;     // mm3 (BRepGProp volume properties)
    double area = 0.0;       // mm2 (sum of face areas)
    gp_Pnt bboxMin{0, 0, 0}; // axis-aligned Bnd_Box corners; carries the
    gp_Pnt bboxMax{0, 0, 0}; //   usual ~1e-7 OCCT bounding-box fringe
    gp_Pnt centroid{0, 0, 0};
};

SolidMetrics solidMetrics(const TopoDS_Shape& shape);

} // namespace solidops
} // namespace viki
