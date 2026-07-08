#pragma once

#include <vector>

#include "Vec2d.h"

namespace viki {
namespace gear {

// Involute spur gear (standard full-depth tooth by default).
struct GearParams {
    double module = 2.0;         // mm — tooth size (pitch dia = module * teeth)
    int teeth = 20;
    double pressureAngleDeg = 20.0;
    double addendumCoef = 1.0;   // addendum = coef * module
    double dedendumCoef = 1.25;  // dedendum = coef * module
    double boreDiameter = 0.0;   // center hole (0 = none)
};

// Everything a designer wants read off the parameters.
struct GearMetrics {
    double pitchDiameter = 0;
    double baseDiameter = 0;
    double outsideDiameter = 0;  // tip / addendum circle
    double rootDiameter = 0;
    double circularPitch = 0;
    double toothThickness = 0;   // at the pitch circle
    double addendum = 0;
    double dedendum = 0;
    double wholeDepth = 0;
    double minTeethNoUndercut = 0;
    bool undercut = false;       // teeth below the undercut limit
};

GearMetrics metrics(const GearParams& p);

// Closed CCW outline of the gear teeth, centered at `center`.
// `chordTol` controls flank tessellation (world units). Guards against the
// self-intersecting "pointed tooth" case for low tooth counts.
std::vector<Vec2d> profile(const GearParams& p, const Vec2d& center,
                           double chordTol);

} // namespace gear
} // namespace viki
