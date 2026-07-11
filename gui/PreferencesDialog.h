#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QString>

class QCheckBox;
class QComboBox;
class QTableWidget;

namespace viki {

// User-remappable input settings, persisted in the app config directory:
// prefs.json (mouse) + shortcuts.json (key -> command line, the same file
// MainWindow::loadShortcuts reads).
struct MousePrefs {
    QString orbit = QStringLiteral("right"); // left | middle | right
    QString pan = QStringLiteral("middle");  // left | middle | right
    bool zoomInvert = false;

    Qt::MouseButton orbitButton() const { return button(orbit, Qt::RightButton); }
    Qt::MouseButton panButton() const { return button(pan, Qt::MiddleButton); }

private:
    static Qt::MouseButton button(const QString& name, Qt::MouseButton fallback)
    {
        if (name.compare(QLatin1String("left"), Qt::CaseInsensitive) == 0)
            return Qt::LeftButton;
        if (name.compare(QLatin1String("middle"), Qt::CaseInsensitive) == 0)
            return Qt::MiddleButton;
        if (name.compare(QLatin1String("right"), Qt::CaseInsensitive) == 0)
            return Qt::RightButton;
        return fallback;
    }
};

// "Preferences…" dialog: 3D mouse mapping + keyboard shortcut table. The
// caller persists and applies the returned values on accept.
class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    PreferencesDialog(const MousePrefs& mouse, const QJsonObject& shortcuts,
                      QWidget* parent = nullptr);

    MousePrefs mousePrefs() const;
    QJsonObject shortcuts() const; // key sequence -> command line

private:
    QComboBox* m_orbit = nullptr;
    QComboBox* m_pan = nullptr;
    QCheckBox* m_zoomInvert = nullptr;
    QTableWidget* m_shortcutTable = nullptr;
};

} // namespace viki
