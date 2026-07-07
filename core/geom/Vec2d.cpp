#include "Vec2d.h"

namespace viki {

Vec2d Vec2d::normalized() const
{
    const double len = length();
    if (nearZero(len))
        return {};
    return {x / len, y / len};
}

Vec2d Vec2d::rotated(double radians) const
{
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    return {x * c - y * s, x * s + y * c};
}

} // namespace viki
