#include "MainWindow.h"

#include <QDockWidget>
#include <QFileDialog>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>

#include "Version.h"
#include "canvas/CanvasWidget.h"
#include "io/NativeStore.h"
#include "panels/CommandBar.h"

namespace viki {

MainWindow::MainWindow()
{
    resize(1280, 800);

    m_canvas = new CanvasWidget(this);
    setCentralWidget(m_canvas);

    m_commandBar = new CommandBar(this);
    auto* dock = new QDockWidget(QStringLiteral("Command"), this);
    dock->setObjectName(QStringLiteral("commandDock"));
    dock->setWidget(m_commandBar);
    dock->setFeatures(QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::BottomDockWidgetArea, dock);

    auto* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    fileMenu->addAction(QStringLiteral("&New"), QKeySequence::New, this, &MainWindow::newFile);
    fileMenu->addAction(QStringLiteral("&Open..."), QKeySequence::Open, this,
                        &MainWindow::openFile);
    fileMenu->addAction(QStringLiteral("&Save"), QKeySequence::Save, this,
                        &MainWindow::saveFile);
    fileMenu->addAction(QStringLiteral("Save &As..."), QKeySequence::SaveAs, this,
                        &MainWindow::saveFileAs);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("&Quit"), QKeySequence::Quit, this, &QWidget::close);

    connect(m_commandBar, &CommandBar::commandEntered, this, &MainWindow::onCommandEntered);
    connect(m_canvas, &CanvasWidget::interaction, this, &MainWindow::onInteraction);

    adoptDocument(std::make_unique<Document>());
    statusBar()->showMessage(
        QStringLiteral("Ready — OCCT %1").arg(QLatin1String(occtVersionString())));
    m_commandBar->focusInput();
}

MainWindow::~MainWindow() = default;

void MainWindow::adoptDocument(std::unique_ptr<Document> doc)
{
    m_selection.clear();
    m_doc = std::move(doc);
    m_ctx = std::make_unique<CommandContext>(*m_doc, m_selection, m_canvas);
    m_processor = std::make_unique<CommandProcessor>(*m_ctx);
    registerBuiltinCommands(*m_processor);
    m_canvas->attach(m_doc.get(), m_processor.get(), &m_selection);
    m_canvas->zoomExtents();
    updateWindowTitle();
    refreshPromptAndMessages();
}

void MainWindow::onCommandEntered(const QString& line)
{
    if (line.isEmpty()) {
        if (m_processor->hasActiveCommand())
            m_processor->provideInput(InputValue::makeFinish());
        refreshPromptAndMessages();
        return;
    }
    const auto r = m_processor->submit(line, /*strict=*/false);
    if (!r.ok)
        m_commandBar->appendHistory(QStringLiteral("! %1").arg(r.error));
    refreshPromptAndMessages();
}

void MainWindow::onInteraction()
{
    refreshPromptAndMessages();
}

void MainWindow::refreshPromptAndMessages()
{
    for (const QString& m : m_ctx->messages())
        m_commandBar->appendHistory(m);
    m_ctx->clearMessages();
    m_commandBar->setPrompt(m_processor->hasActiveCommand()
                                ? m_processor->currentRequest().prompt
                                : QString());
    statusBar()->showMessage(
        QStringLiteral("%1 entities — %2 selected")
            .arg(m_doc->entityCount())
            .arg(m_selection.size()));
}

void MainWindow::updateWindowTitle()
{
    const QString file = m_doc->filePath().isEmpty() ? QStringLiteral("untitled")
                                                     : m_doc->filePath();
    setWindowTitle(QStringLiteral("VikiCAD %1 — %2")
                       .arg(QLatin1String(versionString()), file));
}

void MainWindow::newFile()
{
    adoptDocument(std::make_unique<Document>());
}

void MainWindow::openFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open drawing"), {},
        QStringLiteral("VikiCAD drawings (*.vkd);;All files (*)"));
    if (path.isEmpty())
        return;
    QString error;
    auto doc = NativeStore::load(path, error);
    if (!doc) {
        QMessageBox::warning(this, QStringLiteral("Open failed"), error);
        return;
    }
    adoptDocument(std::move(doc));
}

bool MainWindow::saveTo(const QString& path)
{
    QString error;
    if (!NativeStore::save(*m_doc, path, error)) {
        QMessageBox::warning(this, QStringLiteral("Save failed"), error);
        return false;
    }
    m_doc->setFilePath(path);
    updateWindowTitle();
    statusBar()->showMessage(QStringLiteral("Saved %1").arg(path), 3000);
    return true;
}

void MainWindow::saveFile()
{
    if (m_doc->filePath().isEmpty()) {
        saveFileAs();
        return;
    }
    saveTo(m_doc->filePath());
}

void MainWindow::saveFileAs()
{
    QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save drawing"), {},
        QStringLiteral("VikiCAD drawings (*.vkd)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QLatin1String(".vkd")))
        path += QLatin1String(".vkd");
    saveTo(path);
}

} // namespace viki
