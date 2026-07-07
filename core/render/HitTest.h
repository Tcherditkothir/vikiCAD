#pragma once

#include <vector>

#include "doc/Document.h"

namespace viki {

// Geometric picking over flattened primitives — exact for lines, within the
// flattening chord tolerance for curves (well under a pixel in practice).
namespace hittest {

// Closest entity within `tolerance` (world units) of `point`; 0 if none.
EntityId pick(const Document& doc, const Vec2d& point, double tolerance);

// Entities fully inside `box` (window selection).
std::vector<EntityId> window(const Document& doc, const BBox2d& box);

// Entities inside OR touching `box` (crossing selection).
std::vector<EntityId> crossing(const Document& doc, const BBox2d& box);

} // namespace hittest

} // namespace viki
