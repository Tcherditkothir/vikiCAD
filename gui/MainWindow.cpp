#include "MainWindow.h"

#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QShortcut>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QToolButton>
#include <QAction>

#include "Version.h"
#include "canvas/CanvasWidget.h"
#ifdef VIKICAD_HAS_DXF
#include "io/DxfImporter.h"
#endif
#include "io/NativeStore.h"
#include "io/QueryJson.h"
#include "ipc/RpcServer.h"
#include "occview/OcctViewWidget.h"
#include "panels/CommandBar.h"
#include "panels/LayerPanel.h"
#include "panels/PropertiesPanel.h"
#include "panels/ToolPanels.h"

namespace viki {

MainWindow::MainWindow()
{
    resize(1400, 860);

    m_canvas = new CanvasWidget(this);
    m_viewStack = new QStackedWidget(this);
    m_viewStack->addWidget(m_canvas);
    // The OCCT view (a native window) is created lazily on the first 3D
    // toggle so no native subsurface exists during plain 2D work.
    setCentralWidget(m_viewStack);

    m_commandBar = new CommandBar(this);
    auto* commandDock = new QDockWidget(QStringLiteral("Command"), this);
    commandDock->setObjectName(QStringLiteral("commandDock"));
    commandDock->setWidget(m_commandBar);
    commandDock->setFeatures(QDockWidget::DockWidgetMovable |
                             QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::BottomDockWidgetArea, commandDock);

    m_layerPanel = new LayerPanel(this);
    auto* layerDock = new QDockWidget(QStringLiteral("Layers"), this);
    layerDock->setObjectName(QStringLiteral("layerDock"));
    layerDock->setWidget(m_layerPanel);
    layerDock->setFeatures(QDockWidget::DockWidgetMovable |
                           QDockWidget::DockWidgetFloatable |
                           QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, layerDock);

    m_propsPanel = new PropertiesPanel(this);
    auto* propsDock = new QDockWidget(QStringLiteral("Properties"), this);
    propsDock->setObjectName(QStringLiteral("propsDock"));
    propsDock->setWidget(m_propsPanel);
    propsDock->setFeatures(QDockWidget::DockWidgetMovable |
                           QDockWidget::DockWidgetFloatable |
                           QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, propsDock);

    // --- menus
    auto* viewMenu0 = new QMenu(QStringLiteral("&View"), this);
    viewMenu0->addAction(layerDock->toggleViewAction());
    viewMenu0->addAction(propsDock->toggleViewAction());
    viewMenu0->addAction(commandDock->toggleViewAction());
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
    fileMenu->addAction(QStringLiteral("&Import DXF/DWG..."), this, &MainWindow::importDxfFile);
    fileMenu->addSeparator();
#endif
    fileMenu->addAction(QStringLiteral("&Quit"), QKeySequence::Quit, this, &QWidget::close);
    menuBar()->addMenu(viewMenu0);

    // Grouped command toolbars (each button = the same command as typing it).
    viewMenu0->addSeparator();
    buildToolPanels(this, viewMenu0,
                    [this](const QString& command) { onCommandEntered(command); });

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
    auto* snapBtn = makeToggle(QStringLiteral("SNAP"), true,
               [this](bool on) { m_canvas->snapSettings().enabled = on; });
    // Right-click the SNAP button -> per-type object-snap picker (AutoCAD/
    // nanoCAD "Osnap settings"). Each entry toggles one field of the shared
    // SnapSettings the canvas reads on every cursor move.
    snapBtn->setToolTip(
        QStringLiteral("Object snap (F3) — right-click to choose snap types"));
    snapBtn->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(snapBtn, &QToolButton::customContextMenuRequested, this,
            [this, snapBtn](const QPoint& pos) {
                struct Item { const char* label; bool SnapSettings::* field; };
                static const Item kItems[] = {
                    {"Endpoint", &SnapSettings::endpoint},
                    {"Midpoint", &SnapSettings::midpoint},
                    {"Center", &SnapSettings::center},
                    {"Quadrant", &SnapSettings::quadrant},
                    {"Intersection", &SnapSettings::intersection},
                    {"Perpendicular", &SnapSettings::perpendicular},
                };
                QMenu menu(this);
                auto* header = menu.addAction(QStringLiteral("Object snaps"));
                header->setEnabled(false);
                menu.addSeparator();
                for (const Item& it : kItems) {
                    auto* a = menu.addAction(QString::fromLatin1(it.label));
                    a->setCheckable(true);
                    a->setChecked(m_canvas->snapSettings().*(it.field));
                    auto* canvas = m_canvas;
                    const auto field = it.field;
                    connect(a, &QAction::toggled, this,
                            [canvas, field](bool on) {
                                canvas->snapSettings().*field = on;
                            });
                }
                menu.addSeparator();
                const auto setAll = [this](bool on) {
                    SnapSettings& s = m_canvas->snapSettings();
                    s.endpoint = s.midpoint = s.center = s.quadrant =
                        s.intersection = s.perpendicular = on;
                };
                connect(menu.addAction(QStringLiteral("Select all")),
                        &QAction::triggered, this, [setAll] { setAll(true); });
                connect(menu.addAction(QStringLiteral("Clear all")),
                        &QAction::triggered, this, [setAll] { setAll(false); });
                menu.exec(snapBtn->mapToGlobal(pos));
            });
    auto* gridBtn = makeToggle(QStringLiteral("GRID"), false,
                               [this](bool on) { m_canvas->setGridSnap(on); });
    auto* orthoBtn = makeToggle(QStringLiteral("ORTHO"), false,
                                [this](bool on) { m_canvas->setOrtho(on); });
    auto* polarBtn = makeToggle(QStringLiteral("POLAR"), false,
                                [this](bool on) { m_canvas->setPolar(on); });
    makeToggle(QStringLiteral("3D"), false, [this](bool on) { toggle3D(on); });

    // AutoCAD/nanoCAD function keys.
    const auto bindToggle = [this](Qt::Key key, QToolButton* btn) {
        auto* sc = new QShortcut(QKeySequence(key), this);
        sc->setContext(Qt::ApplicationShortcut);
        connect(sc, &QShortcut::activated, btn, &QToolButton::toggle);
    };
    bindToggle(Qt::Key_F3, snapBtn);
    bindToggle(Qt::Key_F7, gridBtn);
    bindToggle(Qt::Key_F8, orthoBtn);
    bindToggle(Qt::Key_F10, polarBtn);

    m_unitsBtn = new QToolButton(this);
    m_unitsBtn->setText(QStringLiteral("mm"));
    connect(m_unitsBtn, &QToolButton::clicked, this, &MainWindow::toggleUnits);
    statusBar()->addPermanentWidget(m_unitsBtn);

    // --- wiring
    connect(m_commandBar, &CommandBar::commandEntered, this, &MainWindow::onCommandEntered);
    connect(m_canvas, &CanvasWidget::interaction, this, &MainWindow::onInteraction);
    // Type on the canvas -> characters land in the command bar (empty text
    // = Enter with nothing active = repeat last command).
    connect(m_canvas, &CanvasWidget::typed, this, [this](const QString& text) {
        if (text.isEmpty())
            onCommandEntered(QString());
        else
            m_commandBar->beginTyping(text);
    });
    connect(m_commandBar, &CommandBar::cancelRequested, this, [this] {
        if (m_processor->hasActiveCommand())
            m_processor->cancelActive();
        refreshPromptAndMessages();
        m_canvas->update();
    });
    // Space = Enter, except while a command asks for free text.
    m_commandBar->setSpaceSubmits([this] {
        return !(m_processor && m_processor->hasActiveCommand() &&
                 m_processor->currentRequest().kind == InputKind::Text);
    });
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

    // JSON-RPC server for agents (vikicad-cli connect ...).
    m_rpc = new RpcServer(
        [this](const QString& method, const QJsonObject& params) {
            return handleRpc(method, params);
        },
        this);
    if (m_rpc->start())
        m_commandBar->appendHistory(
            QStringLiteral("IPC: listening on local socket 'vikicad'"));

    loadShortcuts();
    m_commandBar->focusInput();
}

void MainWindow::toggle3D(bool on)
{
    if (on) {
        if (!m_occtView) {
            m_occtView = new OcctViewWidget(this);
            m_viewStack->addWidget(m_occtView);
        }
        m_occtView->refreshFrom(*m_doc);
        m_viewStack->setCurrentWidget(m_occtView);
        if (!m_occtView->isReady())
            m_commandBar->appendHistory(
                QStringLiteral("3D view unavailable (no OpenGL/X11 in this session)"));
    } else {
        m_viewStack->setCurrentWidget(m_canvas);
        m_canvas->markDocumentDirty();
    }
}

QJsonObject MainWindow::handleRpc(const QString& method, const QJsonObject& params)
{
    if (method == QLatin1String("ping"))
        return {{QStringLiteral("pong"), true}};
    if (method == QLatin1String("exec")) {
        const QString line = params[QStringLiteral("line")].toString();
        const auto r = m_processor->submit(line, /*strict=*/true);
        refreshPromptAndMessages();
        m_canvas->markDocumentDirty();
        if (!r.ok)
            return {{QStringLiteral("error"), r.error}};
        return {{QStringLiteral("ok"), true},
                {QStringLiteral("entityCount"), qint64(m_doc->entityCount())}};
    }
    if (method == QLatin1String("query")) {
        const QString kind = params[QStringLiteral("kind")].toString();
        QJsonObject result{{QStringLiteral("count"), qint64(m_doc->entityCount())}};
        if (kind.isEmpty() || kind == QLatin1String("entities"))
            result[QStringLiteral("entities")] = queryjson::entitiesJson(*m_doc);
        if (kind == QLatin1String("layers"))
            result[QStringLiteral("layers")] = queryjson::layersJson(*m_doc);
        if (kind == QLatin1String("bounds"))
            result[QStringLiteral("bounds")] = queryjson::boundsJson(*m_doc);
        if (kind == QLatin1String("notes"))
            result[QStringLiteral("notes")] = queryjson::notesJson(*m_doc);
        if (kind == QLatin1String("blocks"))
            result[QStringLiteral("blocks")] = queryjson::blocksJson(*m_doc);
        if (kind == QLatin1String("layouts"))
            result[QStringLiteral("layouts")] = queryjson::layoutsJson(*m_doc);
        return result;
    }
    if (method == QLatin1String("open")) {
        const QString path = params[QStringLiteral("path")].toString();
        QString error;
        auto doc = NativeStore::load(path, error);
        if (!doc)
            return {{QStringLiteral("error"), error}};
        adoptDocument(std::move(doc));
        return {{QStringLiteral("ok"), true}};
    }
    if (method == QLatin1String("save")) {
        QString path = params[QStringLiteral("path")].toString();
        if (path.isEmpty())
            path = m_doc->filePath();
        if (path.isEmpty())
            return {{QStringLiteral("error"), QStringLiteral("no file path")}};
        QString error;
        if (!NativeStore::save(*m_doc, path, error))
            return {{QStringLiteral("error"), error}};
        m_doc->setFilePath(path);
        updateWindowTitle();
        return {{QStringLiteral("ok"), true}, {QStringLiteral("savedTo"), path}};
    }
    if (method == QLatin1String("screenshot")) {
        const QString path = params[QStringLiteral("path")].toString();
        if (path.isEmpty())
            return {{QStringLiteral("error"), QStringLiteral("path required")}};
        const QPixmap shot = m_canvas->grab();
        if (!shot.save(path))
            return {{QStringLiteral("error"), QStringLiteral("cannot write %1").arg(path)}};
        return {{QStringLiteral("ok"), true}, {QStringLiteral("savedTo"), path}};
    }
    return {{QStringLiteral("error"), QStringLiteral("unknown method: %1").arg(method)}};
}

void MainWindow::loadShortcuts()
{
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/shortcuts.json");
    QFile file(path);
    if (!file.exists()) {
        // Seed a commented-by-example default file.
        const QJsonObject defaults{{QStringLiteral("Ctrl+Z"), QStringLiteral("UNDO")},
                                   {QStringLiteral("Ctrl+Y"), QStringLiteral("REDO")},
                                   {QStringLiteral("Ctrl+Shift+E"), QStringLiteral("ZOOM E")},
                                   {QStringLiteral("F2"), QStringLiteral("ZOOM E")}};
        if (file.open(QIODevice::WriteOnly))
            file.write(QJsonDocument(defaults).toJson(QJsonDocument::Indented));
        file.close();
    }
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject map = QJsonDocument::fromJson(file.readAll()).object();
    for (auto it = map.begin(); it != map.end(); ++it) {
        const QString commandLine = it.value().toString();
        auto* shortcut = new QShortcut(QKeySequence(it.key()), this);
        shortcut->setContext(Qt::ApplicationShortcut);
        connect(shortcut, &QShortcut::activated, this, [this, commandLine] {
            onCommandEntered(commandLine);
        });
    }
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
        if (m_processor->hasActiveCommand()) {
            m_processor->provideInput(InputValue::makeFinish());
        } else if (!m_lastCommand.isEmpty()) {
            // AutoCAD: Enter/Space on an empty prompt repeats the last command.
            m_commandBar->appendHistory(
                QStringLiteral("> %1 (repeat)").arg(m_lastCommand));
            m_processor->submit(m_lastCommand, /*strict=*/false);
        }
        refreshPromptAndMessages();
        m_canvas->update();
        return;
    }
    const bool startsCommand = !m_processor->hasActiveCommand();
    const auto r = m_processor->submit(line, /*strict=*/false);
    if (!r.ok)
        m_commandBar->appendHistory(QStringLiteral("! %1").arg(r.error));
    else if (startsCommand)
        m_lastCommand = line.section(QLatin1Char(' '), 0, 0).toUpper();
    refreshPromptAndMessages();
    m_canvas->markDocumentDirty(); // typed points can land mid-transaction too
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

void MainWindow::openPath(const QString& path)
{
    QString error;
    if (path.endsWith(QLatin1String(".vkd"), Qt::CaseInsensitive)) {
        auto doc = NativeStore::load(path, error);
        if (doc)
            adoptDocument(std::move(doc));
        else
            m_commandBar->appendHistory(QStringLiteral("! %1").arg(error));
        return;
    }
#ifdef VIKICAD_HAS_DXF
    if (path.endsWith(QLatin1String(".dxf"), Qt::CaseInsensitive) ||
        path.endsWith(QLatin1String(".dwg"), Qt::CaseInsensitive)) {
        DxfImportResult r = path.endsWith(QLatin1String(".dwg"), Qt::CaseInsensitive)
                                ? importDwg(path)
                                : importDxf(path);
        if (r.ok) {
            adoptDocument(std::move(r.document));
            m_commandBar->appendHistory(
                QStringLiteral("Imported %1 entities from %2").arg(r.imported).arg(path));
        } else {
            m_commandBar->appendHistory(QStringLiteral("! %1").arg(r.error));
        }
        return;
    }
#endif
    m_commandBar->appendHistory(QStringLiteral("! unsupported file: %1").arg(path));
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
        this, QStringLiteral("Import DXF/DWG"), {},
        QStringLiteral("CAD drawings (*.dxf *.dwg);;All files (*)"));
    if (path.isEmpty())
        return;
    DxfImportResult r = path.endsWith(QLatin1String(".dwg"), Qt::CaseInsensitive)
                            ? importDwg(path)
                            : importDxf(path);
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
