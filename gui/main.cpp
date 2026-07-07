#include "MainWindow.h"

#include <QApplication>

#include "Theme.h"
#include "Version.h"

int main(int argc, char** argv)
{
    // Run on X11/XWayland: the OCCT 3D view binds through Xw_Window, and
    // Qt-Wayland dies with a wl_subsurface protocol error when docks float
    // around native child windows. Overridable via QT_QPA_PLATFORM.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "xcb");
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("VikiCAD"));
    viki::applyDarkTheme(app);
    QApplication::setApplicationVersion(QString::fromLatin1(viki::versionString()));

    viki::MainWindow window;
    window.show();
    const QStringList args = QApplication::arguments();
    if (args.size() > 1)
        window.openPath(args.at(1));
    return QApplication::exec();
}
