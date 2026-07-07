#include "Entity.h"

#include <QJsonArray>

namespace viki {

QJsonValue ColorSpec::toJson() const
{
    if (byLayer)
        return QStringLiteral("bylayer");
    return QStringLiteral("#%1").arg(rgb, 6, 16, QLatin1Char('0'));
}

ColorSpec ColorSpec::fromJson(const QJsonValue& v)
{
    ColorSpec c;
    const QString s = v.toString();
    if (s.startsWith(QLatin1Char('#'))) {
        bool ok = false;
        const uint32_t rgb = s.mid(1).toUInt(&ok, 16);
        if (ok) {
            c.byLayer = false;
            c.rgb = rgb;
        }
    }
    return c;
}

QJsonObject Entity::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("type")] = QLatin1String(typeName());
    obj[QStringLiteral("layer")] = qint64(m_layerId);
    obj[QStringLiteral("color")] = m_color.toJson();
    QJsonObject geom;
    geomToJson(geom);
    obj[QStringLiteral("geom")] = geom;
    return obj;
}

void Entity::fromJson(const QJsonObject& obj)
{
    m_layerId = obj[QStringLiteral("layer")].toInteger(0);
    m_color = ColorSpec::fromJson(obj[QStringLiteral("color")]);
    geomFromJson(obj[QStringLiteral("geom")].toObject());
}

QJsonArray pointToJson(const Vec2d& p)
{
    return QJsonArray{p.x, p.y};
}

Vec2d pointFromJson(const QJsonValue& v)
{
    const QJsonArray a = v.toArray();
    return {a.at(0).toDouble(), a.at(1).toDouble()};
}

} // namespace viki
