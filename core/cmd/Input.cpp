#include "Input.h"

namespace viki {

std::optional<Vec2d> parsePointToken(const QString& token, const Vec2d& lastPoint)
{
    QString t = token.trimmed();
    bool relative = false;
    if (t.startsWith(QLatin1Char('@'))) {
        relative = true;
        t = t.mid(1);
    }

    // Polar: dist<angle_deg (relative to lastPoint when @, absolute origin otherwise).
    const int lt = t.indexOf(QLatin1Char('<'));
    if (lt >= 0) {
        bool okDist = false, okAng = false;
        const double dist = t.left(lt).toDouble(&okDist);
        const double angDeg = t.mid(lt + 1).toDouble(&okAng);
        if (!okDist || !okAng)
            return std::nullopt;
        const Vec2d offset = Vec2d::polar(dist, angDeg * M_PI / 180.0);
        return relative ? lastPoint + offset : offset;
    }

    const int comma = t.indexOf(QLatin1Char(','));
    if (comma < 0)
        return std::nullopt;
    bool okX = false, okY = false;
    const double x = t.left(comma).toDouble(&okX);
    const double y = t.mid(comma + 1).toDouble(&okY);
    if (!okX || !okY)
        return std::nullopt;
    const Vec2d p{x, y};
    return relative ? lastPoint + p : p;
}

} // namespace viki
