#pragma once

#include <QString>

namespace viki {

enum class DisplayUnits { Millimeters, Inches };

// M0 stub. The real Document (entity ownership, layers, transaction journal)
// lands in M1; only what the skeleton targets need exists here.
class Document {
public:
    Document() = default;

    QString name() const { return m_name; }
    void setName(const QString& name) { m_name = name; }

    // All stored geometry is always in mm; this only affects formatting/input.
    DisplayUnits displayUnits() const { return m_displayUnits; }
    void setDisplayUnits(DisplayUnits u) { m_displayUnits = u; }

private:
    QString m_name = QStringLiteral("untitled");
    DisplayUnits m_displayUnits = DisplayUnits::Millimeters;
};

} // namespace viki
