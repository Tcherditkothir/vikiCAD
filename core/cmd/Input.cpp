#include "Input.h"

namespace viki {

std::optional<double> parseLengthToken(const QString& token, double unitFactor)
{
    QString t = token.trimmed();
    double factor = unitFactor;
    if (t.endsWith(QLatin1Char('"'))) {
        factor = 25.4;
        t.chop(1);
    } else if (t.endsWith(QLatin1String("in"), Qt::CaseInsensitive)) {
        factor = 25.4;
        t.chop(2);
    } else if (t.endsWith(QLatin1String("mm"), Qt::CaseInsensitive)) {
        factor = 1.0;
        t.chop(2);
    }
    bool ok = false;
    const double v = t.toDouble(&ok);
    if (!ok)
        return std::nullopt;
    return v * factor;
}

std::optional<Vec2d> parsePointToken(const QString& token, const Vec2d& lastPoint,
                                     double unitFactor)
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
        const auto dist = parseLengthToken(t.left(lt), unitFactor);
        bool okAng = false;
        const double angDeg = t.mid(lt + 1).toDouble(&okAng);
        if (!dist || !okAng)
            return std::nullopt;
        const Vec2d offset = Vec2d::polar(*dist, angDeg * M_PI / 180.0);
        return relative ? lastPoint + offset : offset;
    }

    const int comma = t.indexOf(QLatin1Char(','));
    if (comma < 0)
        return std::nullopt;
    const auto x = parseLengthToken(t.left(comma), unitFactor);
    const auto y = parseLengthToken(t.mid(comma + 1), unitFactor);
    if (!x || !y)
        return std::nullopt;
    const Vec2d p{*x, *y};
    return relative ? lastPoint + p : p;
}

} // namespace viki
