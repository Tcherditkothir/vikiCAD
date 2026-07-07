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
    const QStringList args = QApplication::arguments();
    if (args.size() > 1)
        window.openPath(args.at(1));
    return QApplication::exec();
}
