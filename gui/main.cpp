#include "MainWindow.h"

#include <QApplication>

#include "Theme.h"
#include "Version.h"

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("VikiCAD"));
    viki::applyDarkTheme(app);
    QApplication::setApplicationVersion(QString::fromLatin1(viki::versionString()));

    viki::MainWindow window;
    window.show();
    return QApplication::exec();
}
