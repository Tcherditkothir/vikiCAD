#include "MainWindow.h"

#include <QDockWidget>
#include <QStatusBar>

#include "Version.h"
#include "canvas/CanvasWidget.h"
#include "panels/CommandBar.h"

namespace viki {

MainWindow::MainWindow()
{
    setWindowTitle(QStringLiteral("VikiCAD %1").arg(QLatin1String(versionString())));
    resize(1280, 800);

    m_canvas = new CanvasWidget(this);
    setCentralWidget(m_canvas);

    m_commandBar = new CommandBar(this);
    auto* dock = new QDockWidget(QStringLiteral("Command"), this);
    dock->setObjectName(QStringLiteral("commandDock"));
    dock->setWidget(m_commandBar);
    dock->setFeatures(QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::BottomDockWidgetArea, dock);

    connect(m_commandBar, &CommandBar::commandEntered,
            this, &MainWindow::onCommandEntered);

    statusBar()->showMessage(
        QStringLiteral("Ready — OCCT %1").arg(QLatin1String(occtVersionString())));
}

void MainWindow::onCommandEntered(const QString& line)
{
    // M1 wires this into CommandProcessor; for now echo so the loop is visible.
    m_commandBar->appendHistory(QStringLiteral("Command: %1 (no commands yet)").arg(line));
}

} // namespace viki
