#include "Xform2d.h"

namespace viki {

Xform2d Xform2d::rotation(double radians, const Vec2d& center)
{
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    // T(center) * R * T(-center)
    return {c, s, -s, c,
            center.x - c * center.x + s * center.y,
            center.y - s * center.x - c * center.y};
}

Xform2d Xform2d::scaling(double f, const Vec2d& center)
{
    return {f, 0, 0, f, center.x * (1 - f), center.y * (1 - f)};
}

} // namespace viki
