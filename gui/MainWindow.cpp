#include "MainWindow.h"

#include <QDockWidget>
#include <QFileDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QToolButton>

#include "Version.h"
#include "canvas/CanvasWidget.h"
#ifdef VIKICAD_HAS_DXF
#include "io/DxfImporter.h"
#endif
#include "io/NativeStore.h"
#include "panels/CommandBar.h"
#include "panels/LayerPanel.h"
#include "panels/PropertiesPanel.h"

namespace viki {

MainWindow::MainWindow()
{
    resize(1400, 860);

    m_canvas = new CanvasWidget(this);
    setCentralWidget(m_canvas);

    m_commandBar = new CommandBar(this);
    auto* commandDock = new QDockWidget(QStringLiteral("Command"), this);
    commandDock->setObjectName(QStringLiteral("commandDock"));
    commandDock->setWidget(m_commandBar);
    commandDock->setFeatures(QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::BottomDockWidgetArea, commandDock);

    m_layerPanel = new LayerPanel(this);
    auto* layerDock = new QDockWidget(QStringLiteral("Layers"), this);
    layerDock->setObjectName(QStringLiteral("layerDock"));
    layerDock->setWidget(m_layerPanel);
    addDockWidget(Qt::RightDockWidgetArea, layerDock);

    m_propsPanel = new PropertiesPanel(this);
    auto* propsDock = new QDockWidget(QStringLiteral("Properties"), this);
    propsDock->setObjectName(QStringLiteral("propsDock"));
    propsDock->setWidget(m_propsPanel);
    addDockWidget(Qt::RightDockWidgetArea, propsDock);

    // --- menus
    auto* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    fileMenu->addAction(QStringLiteral("&New"), QKeySequence::New, this, &MainWindow::newFile);
    fileMenu->addAction(QStringLiteral("&Open..."), QKeySequence::Open, this,
                        &MainWindow::openFile);
    fileMenu->addAction(QStringLiteral("&Save"), QKeySequence::Save, this,
                        &MainWindow::saveFile);
    fileMenu->addAction(QStringLiteral("Save &As..."), QKeySequence::SaveAs, this,
                        &MainWindow::saveFileAs);
    fileMenu->addSeparator();
#ifdef VIKICAD_HAS_DXF
    fileMenu->addAction(QStringLiteral("&Import DXF..."), this, &MainWindow::importDxfFile);
    fileMenu->addSeparator();
#endif
    fileMenu->addAction(QStringLiteral("&Quit"), QKeySequence::Quit, this, &QWidget::close);

    // --- status bar: coords + mode toggles + units
    m_coordLabel = new QLabel(QStringLiteral("0.00, 0.00"), this);
    m_coordLabel->setMinimumWidth(160);
    statusBar()->addWidget(m_coordLabel);

    const auto makeToggle = [this](const QString& text, bool checked,
                                   const std::function<void(bool)>& fn) {
        auto* btn = new QToolButton(this);
        btn->setText(text);
        btn->setCheckable(true);
        btn->setChecked(checked);
        connect(btn, &QToolButton::toggled, this, fn);
        statusBar()->addPermanentWidget(btn);
        return btn;
    };
    makeToggle(QStringLiteral("SNAP"), true,
               [this](bool on) { m_canvas->snapSettings().enabled = on; });
    makeToggle(QStringLiteral("GRID"), false, [this](bool on) { m_canvas->setGridSnap(on); });
    makeToggle(QStringLiteral("ORTHO"), false, [this](bool on) { m_canvas->setOrtho(on); });
    makeToggle(QStringLiteral("POLAR"), false, [this](bool on) { m_canvas->setPolar(on); });

    m_unitsBtn = new QToolButton(this);
    m_unitsBtn->setText(QStringLiteral("mm"));
    connect(m_unitsBtn, &QToolButton::clicked, this, &MainWindow::toggleUnits);
    statusBar()->addPermanentWidget(m_unitsBtn);

    // --- wiring
    connect(m_commandBar, &CommandBar::commandEntered, this, &MainWindow::onCommandEntered);
    connect(m_canvas, &CanvasWidget::interaction, this, &MainWindow::onInteraction);
    connect(m_canvas, &CanvasWidget::cursorMoved, this, [this](double x, double y) {
        const bool inches = m_doc && m_doc->displayUnits() == DisplayUnits::Inches;
        const double f = inches ? 1.0 / 25.4 : 1.0;
        m_coordLabel->setText(QStringLiteral("%1, %2 %3")
                                  .arg(x * f, 0, 'f', inches ? 4 : 2)
                                  .arg(y * f, 0, 'f', inches ? 4 : 2)
                                  .arg(inches ? QStringLiteral("in") : QStringLiteral("mm")));
    });
    connect(m_layerPanel, &LayerPanel::layersChanged, this, [this] {
        m_canvas->markDocumentDirty();
        m_propsPanel->refresh();
    });
    connect(m_propsPanel, &PropertiesPanel::propertiesApplied, this, [this] {
        m_canvas->markDocumentDirty();
    });

    adoptDocument(std::make_unique<Document>());
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
    m_layerPanel->attach(m_doc.get());
    m_propsPanel->attach(m_doc.get(), &m_selection);
    m_doc->addChangeListener([this] { m_layerPanel->refresh(); });
    m_canvas->zoomExtents();
    updateWindowTitle();
    updateUnitsButton();
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
    m_propsPanel->refresh();
}

void MainWindow::refreshPromptAndMessages()
{
    for (const QString& m : m_ctx->messages())
        m_commandBar->appendHistory(m);
    m_ctx->clearMessages();
    m_commandBar->setPrompt(m_processor->hasActiveCommand()
                                ? m_processor->currentRequest().prompt
                                : QString());
    statusBar()->showMessage(QStringLiteral("%1 entities — %2 selected")
                                 .arg(m_doc->entityCount())
                                 .arg(m_selection.size()),
                             2000);
}

void MainWindow::updateWindowTitle()
{
    const QString file = m_doc->filePath().isEmpty() ? QStringLiteral("untitled")
                                                     : m_doc->filePath();
    setWindowTitle(QStringLiteral("VikiCAD %1 — %2")
                       .arg(QLatin1String(versionString()), file));
}

void MainWindow::updateUnitsButton()
{
    m_unitsBtn->setText(m_doc->displayUnits() == DisplayUnits::Inches
                            ? QStringLiteral("in")
                            : QStringLiteral("mm"));
}

void MainWindow::toggleUnits()
{
    m_doc->setDisplayUnits(m_doc->displayUnits() == DisplayUnits::Inches
                               ? DisplayUnits::Millimeters
                               : DisplayUnits::Inches);
    updateUnitsButton();
    m_commandBar->appendHistory(
        QStringLiteral("Units: %1")
            .arg(m_doc->displayUnits() == DisplayUnits::Inches ? QStringLiteral("inches")
                                                               : QStringLiteral("millimeters")));
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

void MainWindow::importDxfFile()
{
#ifdef VIKICAD_HAS_DXF
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import DXF"), {},
        QStringLiteral("DXF drawings (*.dxf);;All files (*)"));
    if (path.isEmpty())
        return;
    DxfImportResult r = importDxf(path);
    if (!r.ok) {
        QMessageBox::warning(this, QStringLiteral("Import failed"), r.error);
        return;
    }
    adoptDocument(std::move(r.document));
    QString msg = QStringLiteral("Imported %1 entities from %2").arg(r.imported).arg(path);
    if (r.skipped > 0)
        msg += QStringLiteral(" (%1 skipped: %2)")
                   .arg(r.skipped)
                   .arg(r.skippedTypes.join(QStringLiteral(", ")));
    m_commandBar->appendHistory(msg);
#endif
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
