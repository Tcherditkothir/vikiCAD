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
    // Extras first: reserved keys below always win on collision.
    QJsonObject obj = m_extra;
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
    // Unknown top-level keys ride along untouched (importer flags, forward
    // compatibility): everything but the reserved four.
    m_extra = QJsonObject();
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (it.key() == QLatin1String("type") || it.key() == QLatin1String("layer") ||
            it.key() == QLatin1String("color") || it.key() == QLatin1String("geom"))
            continue;
        m_extra[it.key()] = it.value();
    }
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
