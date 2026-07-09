#pragma once

#include <QJsonObject>
#include <QString>

namespace viki {

// Deliberately small dimension style — a dozen knobs, not AutoCAD's 80.
struct DimStyle {
    QString name = QStringLiteral("Standard");
    double textHeight = 3.5;  // mm
    double arrowSize = 2.5;   // mm
    double extOffset = 1.5;   // gap between def point and extension line
    double extBeyond = 1.25;  // extension line past the dimension line
    double textGap = 1.0;     // dimension line to text
    int decimals = 2;
    QString suffix;           // appended to the measurement text
    // DXF DIMPOST (code 3): prefix/suffix template around the measurement.
    // A "<>" placeholder marks where the value goes; without it the whole
    // string is treated as a plain suffix (e.g. " mm").
    QString dimpost;

    QJsonObject toJson() const
    {
        return {{QStringLiteral("name"), name},
                {QStringLiteral("text_height"), textHeight},
                {QStringLiteral("arrow_size"), arrowSize},
                {QStringLiteral("ext_offset"), extOffset},
                {QStringLiteral("ext_beyond"), extBeyond},
                {QStringLiteral("text_gap"), textGap},
                {QStringLiteral("decimals"), decimals},
                {QStringLiteral("suffix"), suffix},
                {QStringLiteral("dimpost"), dimpost}};
    }
    static DimStyle fromJson(const QJsonObject& o)
    {
        DimStyle s;
        s.name = o[QStringLiteral("name")].toString(s.name);
        s.textHeight = o[QStringLiteral("text_height")].toDouble(s.textHeight);
        s.arrowSize = o[QStringLiteral("arrow_size")].toDouble(s.arrowSize);
        s.extOffset = o[QStringLiteral("ext_offset")].toDouble(s.extOffset);
        s.extBeyond = o[QStringLiteral("ext_beyond")].toDouble(s.extBeyond);
        s.textGap = o[QStringLiteral("text_gap")].toDouble(s.textGap);
        s.decimals = o[QStringLiteral("decimals")].toInt(s.decimals);
        s.suffix = o[QStringLiteral("suffix")].toString();
        s.dimpost = o[QStringLiteral("dimpost")].toString();
        return s;
    }

    // Apply the DIMPOST template to an already-formatted measurement string.
    QString applyDimpost(const QString& value) const
    {
        if (dimpost.isEmpty())
            return value;
        if (dimpost.contains(QLatin1String("<>"))) {
            QString r = dimpost;
            r.replace(QLatin1String("<>"), value);
            return r;
        }
        return value + dimpost; // plain suffix
    }
};

} // namespace viki
