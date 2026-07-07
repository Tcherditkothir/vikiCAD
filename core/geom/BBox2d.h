#pragma once

#include <algorithm>
#include <limits>

#include "Vec2d.h"

namespace viki {

// Axis-aligned bounding box. Default-constructed box is empty (invalid).
struct BBox2d {
    Vec2d min{std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
    Vec2d max{std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest()};

    BBox2d() = default;
    BBox2d(const Vec2d& a, const Vec2d& b)
        : min{std::min(a.x, b.x), std::min(a.y, b.y)},
          max{std::max(a.x, b.x), std::max(a.y, b.y)} {}

    bool isValid() const { return min.x <= max.x && min.y <= max.y; }
    Vec2d center() const { return (min + max) * 0.5; }
    double width() const { return max.x - min.x; }
    double height() const { return max.y - min.y; }

    void expand(const Vec2d& p)
    {
        min.x = std::min(min.x, p.x); min.y = std::min(min.y, p.y);
        max.x = std::max(max.x, p.x); max.y = std::max(max.y, p.y);
    }
    void expand(const BBox2d& o)
    {
        if (!o.isValid()) return;
        expand(o.min); expand(o.max);
    }
    BBox2d inflated(double d) const
    {
        BBox2d b = *this;
        b.min -= Vec2d{d, d}; b.max += Vec2d{d, d};
        return b;
    }

    bool contains(const Vec2d& p) const
    {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y;
    }
    bool contains(const BBox2d& o) const
    {
        return o.isValid() && contains(o.min) && contains(o.max);
    }
    bool intersects(const BBox2d& o) const
    {
        return isValid() && o.isValid() &&
               min.x <= o.max.x && max.x >= o.min.x &&
               min.y <= o.max.y && max.y >= o.min.y;
    }
};

} // namespace viki
