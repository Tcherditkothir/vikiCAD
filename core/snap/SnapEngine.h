#pragma once

#include <optional>

#include "doc/Document.h"

namespace viki {

struct SnapSettings {
    bool enabled = true;
    bool endpoint = true;
    bool node = true;
    bool midpoint = true;
    bool center = true;
    bool quadrant = true;
    bool intersection = true;
    bool perpendicular = true;
    bool tangent = true;
    bool nearest = true;
};

struct SnapResult {
    Vec2d point;
    SnapKind kind;
    EntityId entity = kInvalidEntityId;
};

// Object-snap query around `cursor` (tolerance in world units).
// `perpBase`: the rubber-band base point, enables perpendicular AND tangent snap.
// Priority: Endpoint > Node > Intersection > Midpoint > Center > Quadrant >
// Perp > Tangent > Nearest; within a kind, nearest to cursor wins.
std::optional<SnapResult> snapQuery(const Document& doc, const Vec2d& cursor,
                                    double tolerance, const SnapSettings& settings,
                                    const std::optional<Vec2d>& perpBase);

} // namespace viki
