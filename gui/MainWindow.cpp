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
#include <QToolBar>
#include <QToolButton>
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QTabWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "Version.h"
#include "canvas/CanvasWidget.h"
#ifdef VIKICAD_HAS_DXF
#include "io/DxfExporter.h"
#include "io/DxfImporter.h"
#endif
#include "io/ExcellonWriter.h"
#include "io/GerberKit.h"
#include "io/GerberKitWriter.h"
#include "io/NativeStore.h"
#include "io/ObjIo.h"
#include "io/StepIo.h"
#include "io/StlIo.h"
#include "io/QueryJson.h"
#include "ipc/RpcServer.h"
#include "occview/OcctViewWidget.h"
#include "panels/AssemblyPanel.h"
#include "panels/CommandBar.h"
#include "panels/LayerPanel.h"
#include "panels/PropertiesPanel.h"
#include "solid/FeatureParams.h"
#include "solid/SolidEntity.h"
#include "solid/SolidOps.h"

#include <QFileInfo>
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

    m_assemblyPanel = new AssemblyPanel(this);
    auto* asmDock = new QDockWidget(QStringLiteral("Assembly"), this);
    asmDock->setObjectName(QStringLiteral("assemblyDock"));
    asmDock->setWidget(m_assemblyPanel);
    asmDock->setFeatures(QDockWidget::DockWidgetMovable |
                         QDockWidget::DockWidgetFloatable |
                         QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, asmDock);

    // The three right-side panels stack as TABS of one dock area instead of
    // piling on top of each other; each can still be floated/closed on its
    // own. Properties in front by default.
    tabifyDockWidget(layerDock, propsDock);
    tabifyDockWidget(propsDock, asmDock);
    propsDock->raise();

    connect(m_assemblyPanel, &AssemblyPanel::selectionChanged, this, [this] {
        m_canvas->markDocumentDirty();
        m_propsPanel->refresh(); // show the selected solid's properties
        sync3DView();            // cheap highlight sync (rebuild only if dirty)
    });
    connect(m_assemblyPanel, &AssemblyPanel::featureFocused, this,
            [this](EntityId, int nodeIndex) {
                m_propsPanel->focusFeature(nodeIndex);
            });
    // Every refused panel action gets a one-liner in the history — no silent
    // failures (same contract as PropertiesPanel::feedback).
    connect(m_assemblyPanel, &AssemblyPanel::feedback, this,
            [this](const QString& msg) { m_commandBar->appendHistory(msg); });
    // Panel actions (Combine, Move…) run through the shared processor, exactly
    // as if typed — undo, messages and view refresh come from the same path.
    connect(m_assemblyPanel, &AssemblyPanel::commandRequested, this,
            [this](const QString& line) {
                m_commandBar->appendHistory(QStringLiteral("> %1").arg(line));
                const auto r = m_processor->submit(line, /*strict=*/false);
                if (!r.ok)
                    m_commandBar->appendHistory(QStringLiteral("! %1").arg(r.error));
                refreshPromptAndMessages();
                m_canvas->markDocumentDirty();
                sync3DView();
                m_assemblyPanel->refresh();
            });
    // Opening a sketch from the browser lands in the 2D canvas: the canvas IS
    // the aligned plane view (u,v on the restored work plane).
    connect(m_assemblyPanel, &AssemblyPanel::sketchActivated, this,
            [this] { setView3D(false); });

    // When a panel is torn off (floating) give it real window chrome so it can
    // be maximized / full-screened, not just a frameless floating box.
    const auto makeFloatMaximizable = [](QDockWidget* dock) {
        connect(dock, &QDockWidget::topLevelChanged, dock, [dock](bool floating) {
            if (floating) {
                dock->setWindowFlags(Qt::Window | Qt::CustomizeWindowHint |
                                     Qt::WindowTitleHint |
                                     Qt::WindowMinimizeButtonHint |
                                     Qt::WindowMaximizeButtonHint |
                                     Qt::WindowCloseButtonHint);
                dock->show();
            }
        });
    };
    makeFloatMaximizable(layerDock);
    makeFloatMaximizable(propsDock);

    // --- menus
    auto* viewMenu0 = new QMenu(QStringLiteral("&View"), this);
    viewMenu0->addAction(layerDock->toggleViewAction());
    viewMenu0->addAction(propsDock->toggleViewAction());
    viewMenu0->addAction(asmDock->toggleViewAction());
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
#endif
    fileMenu->addAction(QStringLiteral("Open &Gerber kit..."), this,
                        &MainWindow::openGerberKit);
    fileMenu->addAction(QStringLiteral("Insert STEP as &component..."), this,
                        &MainWindow::insertStepComponent);
    // Leaving the .vkd world: export to the planet's formats. The engines
    // existed since M3/M7 (the CLI always could) — the menu was just missing.
    QMenu* exportMenu = fileMenu->addMenu(QStringLiteral("&Export"));
    exportMenu->addAction(QStringLiteral("STEP (3D solids)..."), this,
                          [this] { exportAs(QStringLiteral("step")); });
#ifdef VIKICAD_HAS_DXF
    exportMenu->addAction(QStringLiteral("DXF (2D drawing)..."), this,
                          [this] { exportAs(QStringLiteral("dxf")); });
#endif
    exportMenu->addAction(QStringLiteral("STL (3D-print mesh)..."), this,
                          [this] { exportAs(QStringLiteral("stl")); });
    exportMenu->addAction(QStringLiteral("OBJ (mesh)..."), this,
                          [this] { exportAs(QStringLiteral("obj")); });
    exportMenu->addSeparator();
    // G3: fabrication outputs — the whole kit into a directory
    // (<docname>.GTL/.GBL/... + .TXT drills) or one layer to one file.
    exportMenu->addAction(QStringLiteral("Gerber kit (directory)..."), this,
                          &MainWindow::exportGerberKitDir);
    exportMenu->addAction(QStringLiteral("Gerber/Excellon layer..."), this,
                          &MainWindow::exportGerberLayerFile);
    fileMenu->addSeparator();
    // Output commands (they run through the processor like typed commands).
    fileMenu->addAction(QStringLiteral("Page &Layout..."), this,
                        [this] { onCommandEntered(QStringLiteral("LAYOUT")); });
    fileMenu->addAction(QStringLiteral("&Plot / Print..."), this,
                        [this] { onCommandEntered(QStringLiteral("PLOT")); });
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("&Preferences..."), this,
                        &MainWindow::openPreferences);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("&Quit"), QKeySequence::Quit, this, &QWidget::close);

    // --- Edit menu: the standard commands live HERE, one binding each
    // (duplicated shortcuts made Qt fire neither — see above).
    auto* editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    QAction* undoAct = editMenu->addAction(QStringLiteral("&Undo"));
    undoAct->setShortcut(QKeySequence::Undo); // Ctrl+Z
    undoAct->setShortcutContext(Qt::ApplicationShortcut);
    connect(undoAct, &QAction::triggered, this,
            [this] { onCommandEntered(QStringLiteral("UNDO")); });
    QAction* redoAct = editMenu->addAction(QStringLiteral("&Redo"));
    redoAct->setShortcuts({QKeySequence::Redo,               // Ctrl+Shift+Z
                           QKeySequence(QStringLiteral("Ctrl+Y"))});
    redoAct->setShortcutContext(Qt::ApplicationShortcut);
    connect(redoAct, &QAction::triggered, this,
            [this] { onCommandEntered(QStringLiteral("REDO")); });
    editMenu->addSeparator();
    QAction* delAct = editMenu->addAction(QStringLiteral("&Delete selection"));
    delAct->setShortcut(QKeySequence::Delete);
    delAct->setShortcutContext(Qt::ApplicationShortcut);
    connect(delAct, &QAction::triggered, this,
            [this] { onCommandEntered(QStringLiteral("ERASE")); });

    viewMenu0->addSeparator();
    viewMenu0->addAction(QStringLiteral("&Zoom..."), this,
                         [this] { onCommandEntered(QStringLiteral("ZOOM")); });
    // CAM stack presets — same BOARDVIEW command as typing it (processor
    // parity): TOP dims the bottom-side layers, BOTTOM dims the top side
    // AND mirrors the view left-right (solder side), ALL restores.
    viewMenu0->addSeparator();
    QMenu* boardMenu = viewMenu0->addMenu(QStringLiteral("&Board view (CAM)"));
    boardMenu->addAction(QStringLiteral("&Top"), this, [this] {
        onCommandEntered(QStringLiteral("BOARDVIEW TOP"));
    });
    boardMenu->addAction(QStringLiteral("&Bottom (mirrored)"), this, [this] {
        onCommandEntered(QStringLiteral("BOARDVIEW BOTTOM"));
    });
    boardMenu->addAction(QStringLiteral("&All layers"), this, [this] {
        onCommandEntered(QStringLiteral("BOARDVIEW ALL"));
    });
    menuBar()->addMenu(viewMenu0);

    // Grouped command menus + the compact tool tab strip above the view
    // (each button = the same command as typing it). One tab per group
    // replaces the old stack of wrap-around toolbars.
    const auto runCommand = [this](const QString& command) {
        onCommandEntered(command);
    };
    buildToolMenus(this, runCommand);
    m_toolTabs = buildToolTabs(this, runCommand);
    // Pin an EXPLICIT arrow cursor on the button areas: cursor inheritance
    // near the native GL view occasionally left the pointer invisible over
    // the top buttons.
    m_toolTabs->setCursor(Qt::ArrowCursor);
    menuBar()->setCursor(Qt::ArrowCursor);

    // Big, unmissable "Finish sketch" button (Fusion-style), pinned to the
    // right corner of the tool tab strip while a sketch is open. Finishing a
    // face sketch returns to the 3D view it came from.
    m_finishSketchBtn = new QPushButton(QStringLiteral("✓ Finish sketch"), this);
    m_finishSketchBtn->setStyleSheet(
        QStringLiteral("QPushButton { background: #2e8b57; color: white; "
                       "font-weight: bold; padding: 4px 14px; border-radius: "
                       "3px; } QPushButton:hover { background: #37a469; }"));
    m_finishSketchBtn->setVisible(false);
    m_toolTabs->setCornerWidget(m_finishSketchBtn, Qt::TopRightCorner);
    connect(m_finishSketchBtn, &QPushButton::clicked, this, [this] {
        // Capture BEFORE closing: updateSketchStatus drops the flag as part
        // of the close itself. Fall back on "the document has solids" so the
        // return to 3D never depends on HOW the sketch was entered.
        bool hasSolids = false;
        for (const EntityId eid : m_doc->drawOrder())
            if (dynamic_cast<const SolidEntity*>(m_doc->entity(eid))) {
                hasSolids = true;
                break;
            }
        const bool backTo3d = m_sketchFrom3d || hasSolids;
        m_sketchFrom3d = false;
        onCommandEntered(QStringLiteral("SKETCH CLOSE"));
        if (backTo3d)
            setView3D(true); // back to the model you were sketching on
    });

    // Views tab: standard camera views + fit + grid for the 3D view (the
    // ViewCube itself lives inside the OCCT viewport). Greyed out until the
    // 3D view is active — same reachability as the old "3D views" toolbar.
    QHBoxLayout* viewsRow = addToolTabPage(m_toolTabs, QStringLiteral("Views"));
    m_viewsTabIndex = m_toolTabs->count() - 1;
    const auto addView = [this, viewsRow](const QString& label, const QString& name) {
        QToolButton* btn = addToolButton(viewsRow, label, QString(),
                                         QStringLiteral("%1 view").arg(label));
        btn->setText(label); // single-line: these are view names, not glyphs
        connect(btn, &QToolButton::clicked, this, [this, name] {
            if (m_occtView)
                m_occtView->setStandardView(name);
        });
    };
    addView(QStringLiteral("Top"), QStringLiteral("TOP"));
    addView(QStringLiteral("Front"), QStringLiteral("FRONT"));
    addView(QStringLiteral("Right"), QStringLiteral("RIGHT"));
    addView(QStringLiteral("Iso"), QStringLiteral("ISO"));
    {
        QToolButton* fit = addToolButton(viewsRow, QStringLiteral("Fit"),
                                         QString(), QStringLiteral("Fit all"));
        fit->setText(QStringLiteral("Fit"));
        connect(fit, &QToolButton::clicked, this, [this] {
            if (m_occtView)
                m_occtView->fitView();
        });
        QToolButton* grid =
            addToolButton(viewsRow, QStringLiteral("Grid"), QString(),
                          QStringLiteral("Toggle the 3D grid"));
        grid->setText(QStringLiteral("Grid"));
        grid->setCheckable(true);
        connect(grid, &QToolButton::toggled, this, [this](bool on) {
            if (m_occtView)
                m_occtView->setGridVisible(on);
        });
    }
    m_toolTabs->setTabEnabled(m_viewsTabIndex, false); // enabled with 3D mode

    // Host the strip in a fixed toolbar row so the View menu gets a single
    // show/hide toggle for the whole thing.
    auto* tabStripBar = new QToolBar(QStringLiteral("Tool tabs"), this);
    tabStripBar->setObjectName(QStringLiteral("toolTabsBar"));
    tabStripBar->setMovable(false);
    tabStripBar->addWidget(m_toolTabs);
    addToolBar(Qt::TopToolBarArea, tabStripBar);
    viewMenu0->addSeparator();
    viewMenu0->addAction(tabStripBar->toggleViewAction());

    // --- Help menu (last, per convention)
    auto* helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    helpMenu->addAction(QStringLiteral("&About VikiCAD"), this, [this] {
        QMessageBox::about(
            this, QStringLiteral("About VikiCAD"),
            QStringLiteral(
                "<h3>VikiCAD %1</h3>"
                "<p>A 2D/3D CAD application — DXF/DWG drafting and "
                "OpenCASCADE solid modeling (built against OCCT %2).</p>"
                "<p>Copyright © 2026 Lex Richer.<br>"
                "Developed with Claude (Anthropic). Built on Qt, "
                "Open CASCADE Technology, libdxfrw and LibreDWG.</p>"
                "<p>This program is free software: you can redistribute it "
                "and/or modify it under the terms of the GNU General Public "
                "License as published by the Free Software Foundation, "
                "either version 3 of the License, or (at your option) any "
                "later version. It is distributed in the hope that it will "
                "be useful, but WITHOUT ANY WARRANTY; without even the "
                "implied warranty of MERCHANTABILITY or FITNESS FOR A "
                "PARTICULAR PURPOSE. See "
                "<a href=\"https://www.gnu.org/licenses/gpl-3.0.html\">"
                "https://www.gnu.org/licenses/gpl-3.0.html</a>.</p>")
                .arg(QString::fromLatin1(versionString()),
                     QString::fromLatin1(occtVersionString())));
    });
    helpMenu->addAction(QStringLiteral("About &Qt"),
                        this, [this] { QMessageBox::aboutQt(this); });

    // --- status bar: coords + mode toggles + units
    m_coordLabel = new QLabel(QStringLiteral("0.00, 0.00"), this);
    m_coordLabel->setMinimumWidth(160);
    statusBar()->addWidget(m_coordLabel);

    // While a sketch is open, its name stays visible here (transient
    // showMessage() texts would hide it).
    m_sketchLabel = new QLabel(this);
    m_sketchLabel->setVisible(false);
    statusBar()->addPermanentWidget(m_sketchLabel);

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
    m_3dButton = makeToggle(QStringLiteral("3D"), false, [this](bool on) { toggle3D(on); });

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

    // Undo/Redo/Delete live in the EDIT MENU (created with the other menus
    // below) — their shortcuts belong to those QActions. The old duplicate
    // QShortcuts here + the same keys in shortcuts.json made Qt declare the
    // bindings AMBIGUOUS and fire NEITHER: Ctrl+Z was dead since day one.

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
    // Right-click on empty space repeats the last command (AutoCAD gesture).
    connect(m_canvas, &CanvasWidget::repeatLastRequested, this,
            [this] { onCommandEntered(QString()); });
    // Double-click an entity -> inline editor (text for now).
    connect(m_canvas, &CanvasWidget::editEntityRequested, this,
            &MainWindow::editEntity);
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
        // Colour / transparency / feature edits must reflect in the 3D view
        // too (they change the document, so this is a real rebuild).
        sync3DView();
    });
    connect(m_propsPanel, &PropertiesPanel::feedback, this,
            [this](const QString& msg) { m_commandBar->appendHistory(msg); });

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
    loadMousePrefs();
    m_commandBar->focusInput();
}

void MainWindow::toggle3D(bool on)
{
    if (on) {
        if (!m_occtView) {
            m_occtView = new OcctViewWidget(this);
            m_occtView->attach(m_doc.get(), m_processor.get(), &m_selection);
            m_occtView->setMouseBindings(m_mousePrefs.orbitButton(),
                                         m_mousePrefs.panButton(),
                                         m_mousePrefs.zoomInvert);
            connect(m_occtView, &OcctViewWidget::interaction, this,
                    &MainWindow::onInteraction);
            // Type over the 3D view -> characters land in the command bar
            // (no need to click it first), same contract as the 2D canvas.
            connect(m_occtView, &OcctViewWidget::typed, this,
                    [this](const QString& text) {
                        if (text.isEmpty())
                            onCommandEntered(QString());
                        else
                            m_commandBar->beginTyping(text);
                    });
            connect(m_occtView, &OcctViewWidget::picked, this,
                    [this](const QString& info) {
                        m_commandBar->appendHistory(info);
                        // Clicking a bore wall selects THE HOLE, not just the
                        // solid: focus its parameters in Properties and its
                        // node in the Assembly tree.
                        const EntityId id = m_occtView->pickedSolid();
                        const auto* solid =
                            dynamic_cast<const SolidEntity*>(m_doc->entity(id));
                        if (solid && solid->features) {
                            const int node = featureForFace(
                                *solid->features, m_occtView->pickedFace());
                            if (node >= 0) {
                                m_propsPanel->focusFeature(node);
                                m_assemblyPanel->focusFeature(id, node);
                                m_commandBar->appendHistory(
                                    QStringLiteral("→ hole %1 selected — edit it "
                                                   "in Properties")
                                        .arg(node));
                            }
                        }
                    });
            connect(m_occtView, &OcctViewWidget::sketchOnFace, this,
                    &MainWindow::beginSketchOnFace);
            // Refused context-menu actions explain themselves in the history.
            connect(m_occtView, &OcctViewWidget::feedback, this,
                    [this](const QString& msg) {
                        m_commandBar->appendHistory(msg);
                    });
            connect(m_occtView, &OcctViewWidget::pushPullFace, this,
                    [this](EntityId id, double dist) {
                        const auto* solid =
                            dynamic_cast<const SolidEntity*>(m_doc->entity(id));
                        if (!solid) {
                            m_commandBar->appendHistory(QStringLiteral(
                                "! push/pull: the picked solid no longer exists"));
                            return;
                        }
                        // Push/pull ON A BORE WALL resizes the hole (Fusion
                        // semantics): pushing outward (+) enlarges the
                        // diameter by 2×distance, pulling inward shrinks it.
                        if (solid->features) {
                            const int node = featureForFace(
                                *solid->features, m_occtView->pickedFace());
                            if (node >= 0) {
                                const double old =
                                    solid->features->nodeAt(node).diameter;
                                const double next = old + 2.0 * dist;
                                if (next <= 0.01) {
                                    m_commandBar->appendHistory(QStringLiteral(
                                        "! push/pull: that would close the "
                                        "hole (diameter %1 → %2)")
                                        .arg(old).arg(next));
                                    return;
                                }
                                TransactionScope scope(
                                    *m_doc, QStringLiteral("HOLE RADIUS"));
                                bool ok = false;
                                try {
                                    if (auto* s = dynamic_cast<SolidEntity*>(
                                            m_doc->beginModify(id))) {
                                        ok = featureparams::set(
                                                 *s->features, node,
                                                 QStringLiteral("diameter"),
                                                 next) &&
                                             s->regenerateFeatures();
                                        m_doc->endModify(id);
                                    }
                                    if (ok)
                                        scope.commit();
                                    else
                                        scope.rollback();
                                } catch (const std::exception&) {
                                    scope.rollback();
                                }
                                m_commandBar->appendHistory(
                                    ok ? QStringLiteral(
                                             "Hole %1: diameter %2 → %3")
                                             .arg(node).arg(old).arg(next)
                                       : QStringLiteral(
                                             "! push/pull: hole regeneration "
                                             "failed"));
                                if (ok) {
                                    sync3DView();
                                    m_propsPanel->refresh();
                                }
                                return;
                            }
                        }
                        const auto res = solidops::pushPullFace(
                            solid->shape(), m_occtView->pickedFace(), dist);
                        if (!res.ok) {
                            m_commandBar->appendHistory(
                                QStringLiteral("! push/pull: %1").arg(res.message));
                            return;
                        }
                        TransactionScope scope(*m_doc, QStringLiteral("PUSHPULL"));
                        try {
                            if (auto* s = dynamic_cast<SolidEntity*>(m_doc->beginModify(id))) {
                                s->setShape(res.shape);
                                m_doc->endModify(id);
                            }
                            scope.commit();
                        } catch (const std::exception& ex) {
                            scope.rollback();
                            m_commandBar->appendHistory(
                                QStringLiteral("! push/pull: %1")
                                    .arg(QString::fromUtf8(ex.what())));
                            return;
                        }
                        m_occtView->refreshFrom(*m_doc);
                        m_commandBar->appendHistory(
                            QStringLiteral("Push/Pull: %1 by %2")
                                .arg(dist > 0 ? QStringLiteral("boss")
                                              : QStringLiteral("pocket"))
                                .arg(dist));
                    });
            connect(m_occtView, &OcctViewWidget::splitByFace, this, [this]() {
                const EntityId id = m_occtView->pickedSolid();
                const auto* solid =
                    dynamic_cast<const SolidEntity*>(m_doc->entity(id));
                if (!solid) {
                    m_commandBar->appendHistory(QStringLiteral(
                        "! split: the picked solid no longer exists"));
                    return;
                }
                // Planar face -> split by its (infinite) plane. CURVED face ->
                // try the face itself as the splitting tool: it works wherever
                // the surface actually crosses the solid, and we only report
                // failure when it truly doesn't make sense (undo covers the
                // rest).
                const auto wp = solidops::planeFromFace(m_occtView->pickedFace());
                const auto pieces =
                    wp ? solidops::splitByPlane(solid->shape(),
                                                gp_Pln(wp->origin, wp->normal))
                       : solidops::splitSolid(solid->shape(),
                                              m_occtView->pickedFace());
                if (pieces.size() < 2) {
                    m_commandBar->appendHistory(QStringLiteral(
                        "! split: this face does not cut through the solid"));
                    return;
                }
                // Every piece inherits the split solid's fields.
                const int64_t layerId = solid->layerId();
                const ColorSpec color = solid->color();
                const QString component = solid->component;
                const double transparency = solid->transparency;
                TransactionScope scope(*m_doc, QStringLiteral("SPLIT"));
                try {
                    m_doc->removeEntity(id);
                    for (const TopoDS_Shape& piece : pieces) {
                        auto e = std::make_unique<SolidEntity>(piece);
                        e->setLayerId(layerId);
                        e->setColor(color);
                        e->component = component;
                        e->transparency = transparency;
                        m_doc->addEntity(std::move(e));
                    }
                    scope.commit();
                } catch (const std::exception& ex) {
                    scope.rollback();
                    m_commandBar->appendHistory(
                        QStringLiteral("! split: %1").arg(QString::fromUtf8(ex.what())));
                    return;
                }
                m_occtView->refreshFrom(*m_doc);
                m_commandBar->appendHistory(
                    QStringLiteral("Split: %1 pieces").arg(pieces.size()));
            });
            // One shared "replace the solid's shape in a transaction" path for
            // the multi-element context-menu operations.
            const auto replaceShape = [this](EntityId id,
                                             const solidops::SolidResult& res,
                                             const QString& tx,
                                             const QString& okMsg) {
                if (!res.ok) {
                    m_commandBar->appendHistory(
                        QStringLiteral("! %1").arg(res.message));
                    return;
                }
                TransactionScope scope(*m_doc, tx);
                try {
                    if (auto* s = dynamic_cast<SolidEntity*>(m_doc->beginModify(id))) {
                        s->setShape(res.shape);
                        m_doc->endModify(id);
                    }
                    scope.commit();
                } catch (const std::exception& ex) {
                    scope.rollback();
                    m_commandBar->appendHistory(
                        QStringLiteral("! %1: %2").arg(tx).arg(QString::fromUtf8(ex.what())));
                    return;
                }
                sync3DView();
                m_commandBar->appendHistory(okMsg);
            };
            connect(m_occtView, &OcctViewWidget::filletSelectedEdges, this,
                    [this, replaceShape](EntityId id, double radius) {
                        const auto* s =
                            dynamic_cast<const SolidEntity*>(m_doc->entity(id));
                        if (!s) {
                            m_commandBar->appendHistory(QStringLiteral(
                                "! fillet: the picked solid no longer exists"));
                            return;
                        }
                        replaceShape(
                            id,
                            solidops::filletEdges(s->shape(),
                                                  m_occtView->pickedEdges(), radius),
                            QStringLiteral("FILLET3D"),
                            QStringLiteral("Fillet: %1 edge(s), r=%2")
                                .arg(m_occtView->pickedEdges().size())
                                .arg(radius));
                    });
            connect(m_occtView, &OcctViewWidget::chamferSelectedEdges, this,
                    [this, replaceShape](EntityId id, double dist) {
                        const auto* s =
                            dynamic_cast<const SolidEntity*>(m_doc->entity(id));
                        if (!s) {
                            m_commandBar->appendHistory(QStringLiteral(
                                "! chamfer: the picked solid no longer exists"));
                            return;
                        }
                        replaceShape(
                            id,
                            solidops::chamferEdges(s->shape(),
                                                   m_occtView->pickedEdges(), dist),
                            QStringLiteral("CHAMFER3D"),
                            QStringLiteral("Chamfer: %1 edge(s), d=%2")
                                .arg(m_occtView->pickedEdges().size())
                                .arg(dist));
                    });
            connect(m_occtView, &OcctViewWidget::shellOpenFaces, this,
                    [this, replaceShape](EntityId id, double thickness) {
                        const auto* s =
                            dynamic_cast<const SolidEntity*>(m_doc->entity(id));
                        if (!s) {
                            m_commandBar->appendHistory(QStringLiteral(
                                "! shell: the picked solid no longer exists"));
                            return;
                        }
                        replaceShape(
                            id,
                            solidops::shellSolid(s->shape(), thickness,
                                                 m_occtView->pickedFaces()),
                            QStringLiteral("SHELL"),
                            QStringLiteral("Shell: t=%1, %2 face(s) open")
                                .arg(thickness)
                                .arg(m_occtView->pickedFaces().size()));
                    });
            // Right-click "Move solid…" / "Move by two points" run through the
            // shared processor, exactly as if typed.
            connect(m_occtView, &OcctViewWidget::commandRequested, this,
                    [this](const QString& line) {
                        m_commandBar->appendHistory(QStringLiteral("> %1").arg(line));
                        onCommandEntered(line);
                    });
            // Right-click on a bore wall: edit THAT hole (parametric).
            const auto editHole = [this](EntityId id, int node,
                                         const std::vector<std::pair<QString, double>>&
                                             params,
                                         const QString& tx) {
                TransactionScope scope(*m_doc, tx);
                bool ok = false;
                QString error = QStringLiteral("regeneration rejected");
                try {
                    if (auto* s = dynamic_cast<SolidEntity*>(m_doc->beginModify(id))) {
                        ok = s->features != nullptr;
                        for (const auto& p : params)
                            ok = ok && featureparams::set(*s->features, node,
                                                          p.first, p.second);
                        ok = ok && s->regenerateFeatures();
                        m_doc->endModify(id);
                    }
                } catch (const std::exception& ex) {
                    ok = false;
                    error = QString::fromUtf8(ex.what());
                }
                if (ok) {
                    scope.commit();
                    sync3DView();
                    m_propsPanel->refresh();
                    m_assemblyPanel->refresh();
                    m_commandBar->appendHistory(QStringLiteral("%1: done").arg(tx));
                } else {
                    scope.rollback();
                    m_commandBar->appendHistory(
                        QStringLiteral("! %1 failed (%2)").arg(tx).arg(error));
                }
            };
            connect(m_occtView, &OcctViewWidget::moveHoleRequested, this,
                    [editHole](EntityId id, int node, double cx, double cy) {
                        editHole(id, node,
                                 {{QStringLiteral("center x"), cx},
                                  {QStringLiteral("center y"), cy}},
                                 QStringLiteral("MOVE HOLE"));
                    });
            connect(m_occtView, &OcctViewWidget::holeDiameterRequested, this,
                    [editHole](EntityId id, int node, double d) {
                        editHole(id, node, {{QStringLiteral("diameter"), d}},
                                 QStringLiteral("HOLE DIAMETER"));
                    });
            m_viewStack->addWidget(m_occtView);
        }
        // The Views tab (built in the constructor) is only usable while the
        // 3D view is active — same reachability as the old "3D views" bar.
        if (m_toolTabs && m_viewsTabIndex >= 0) {
            m_toolTabs->setTabEnabled(m_viewsTabIndex, true);
            m_toolTabs->setCurrentIndex(m_viewsTabIndex);
        }
        m_occtView->refreshFrom(*m_doc);
        m_viewStack->setCurrentWidget(m_occtView);
        if (!m_occtView->isReady())
            m_commandBar->appendHistory(
                QStringLiteral("3D view unavailable (no OpenGL/X11 in this session)"));
    } else {
        if (m_toolTabs && m_viewsTabIndex >= 0) {
            if (m_toolTabs->currentIndex() == m_viewsTabIndex)
                m_toolTabs->setCurrentIndex(0); // back to Draw
            m_toolTabs->setTabEnabled(m_viewsTabIndex, false);
        }
        m_viewStack->setCurrentWidget(m_canvas);
        m_canvas->markDocumentDirty();
    }
}

void MainWindow::setView3D(bool on)
{
    if (m_3dButton && m_3dButton->isChecked() != on)
        m_3dButton->setChecked(on); // fires toggled -> toggle3D
    else
        toggle3D(on);
}

bool MainWindow::documentIsSolidsOnly() const
{
    if (!m_doc)
        return false;
    int solids = 0, flat = 0;
    for (const EntityId id : m_doc->drawOrder()) {
        const Entity* e = m_doc->entity(id);
        if (!e)
            continue;
        if (QLatin1String(e->typeName()) == QLatin1String("solid"))
            ++solids;
        else if (QLatin1String(e->typeName()) != QLatin1String("sticky_note"))
            ++flat;
    }
    return solids > 0 && flat == 0;
}

QJsonObject MainWindow::handleRpc(const QString& method, const QJsonObject& params)
{
    if (method == QLatin1String("ping"))
        return {{QStringLiteral("pong"), true}};
    if (method == QLatin1String("export")) {
        const QString path = params[QStringLiteral("path")].toString();
        if (path.isEmpty())
            return {{QStringLiteral("error"), QStringLiteral("export needs a path")}};
        QString message;
        QStringList warnings;
        const bool ok = exportToPath(path, message,
                                     params[QStringLiteral("layer")].toString(),
                                     &warnings);
        if (!ok)
            return {{QStringLiteral("error"), message}};
        QJsonObject reply{{QStringLiteral("ok"), true},
                          {QStringLiteral("message"), message}};
        if (!warnings.isEmpty())
            reply[QStringLiteral("warnings")] =
                QJsonArray::fromStringList(warnings);
        return reply;
    }
    if (method == QLatin1String("exec")) {
        const QString line = params[QStringLiteral("line")].toString();
        const auto r = m_processor->submit(line, /*strict=*/true);
        // Command output (INSPECT listings, MEASURE results...) goes back to
        // the agent too — capture before refreshPromptAndMessages drains it
        // into the command-bar history.
        QJsonArray messages;
        for (const QString& m : m_ctx->messages())
            messages.append(m);
        refreshPromptAndMessages();
        m_canvas->markDocumentDirty();
        // Commands can change the selection (SELECT) or entity properties:
        // keep the Properties panel live for IPC-driven sessions too.
        m_propsPanel->refresh();
        sync3DView(); // IPC-driven 3D commands must show up too
        if (!r.ok)
            return {{QStringLiteral("error"), r.error},
                    {QStringLiteral("messages"), messages}};
        return {{QStringLiteral("ok"), true},
                {QStringLiteral("entityCount"), qint64(m_doc->entityCount())},
                {QStringLiteral("messages"), messages}};
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
        if (kind == QLatin1String("describe"))
            result[QStringLiteral("describe")] = queryjson::describeJson(*m_doc);
        if (kind == QLatin1String("ui")) {
            // Layout self-description for the gui-smoke harness: the tool tab
            // strip and the tabified right-side docks.
            QJsonArray tabs;
            if (m_toolTabs)
                for (int i = 0; i < m_toolTabs->count(); ++i)
                    tabs.append(m_toolTabs->tabText(i));
            result[QStringLiteral("toolTabs")] = tabs;
            QJsonArray docks;
            if (auto* props =
                    findChild<QDockWidget*>(QStringLiteral("propsDock"))) {
                docks.append(props->windowTitle());
                for (QDockWidget* d : tabifiedDockWidgets(props))
                    docks.append(d->windowTitle());
            }
            result[QStringLiteral("tabbedDocks")] = docks;
            // What the Properties panel currently SHOWS (key/value rows):
            // lets an agent verify the gerber inspector after a SELECT.
            result[QStringLiteral("propRows")] = m_propsPanel->tableRows();
        }
        return result;
    }
    if (method == QLatin1String("open")) {
        const QString path = params[QStringLiteral("path")].toString();
        // Same dispatch as File>Open: .vkd/.dxf/.dwg/.step, and a
        // directory opens as a Gerber kit.
        if (!loadFile(path, /*interactive=*/false))
            return {{QStringLiteral("error"),
                     QStringLiteral("could not open %1").arg(path)}};
        QJsonObject reply{{QStringLiteral("ok"), true}};
        // Mirror the CLI "import" JSON: importer warnings (e.g. "valid but
        // empty" fab files) must reach the IPC caller too, not only the
        // command-bar history.
        if (!m_openWarnings.isEmpty())
            reply[QStringLiteral("warnings")] =
                QJsonArray::fromStringList(m_openWarnings);
        return reply;
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
    if (method == QLatin1String("view3d")) {
        const bool on = params[QStringLiteral("on")].toBool(true);
        setView3D(on);
        return {{QStringLiteral("ok"), true},
                {QStringLiteral("is3d"),
                 m_3dButton ? m_3dButton->isChecked() : false}};
    }
    if (method == QLatin1String("pick3d")) {
        if (!m_occtView || m_viewStack->currentWidget() != m_occtView)
            return {{QStringLiteral("error"), QStringLiteral("not in 3D view")}};
        // Coords in physical view pixels; default to the view centre.
        const int cx = params[QStringLiteral("x")].toInt(-1);
        const int cy = params[QStringLiteral("y")].toInt(-1);
        const QString info = (cx < 0 || cy < 0) ? m_occtView->pickCenter()
                                                : m_occtView->pickAtPhysical(cx, cy);
        return {{QStringLiteral("ok"), true}, {QStringLiteral("picked"), info}};
    }
    if (method == QLatin1String("viewdir")) {
        // The agent's eyes: aim the 3D camera along a standard view
        // (TOP/BOTTOM/FRONT/BACK/LEFT/RIGHT/ISO) — setStandardView also
        // FitAlls, so a following `screenshot` frames the whole scene.
        if (!m_occtView || m_viewStack->currentWidget() != m_occtView)
            return {{QStringLiteral("error"), QStringLiteral("not in 3D view")}};
        const QString name = params[QStringLiteral("name")].toString();
        if (!m_occtView->setStandardView(name))
            return {{QStringLiteral("error"),
                     QStringLiteral("unknown view: %1 (try TOP/BOTTOM/FRONT/"
                                    "BACK/LEFT/RIGHT/ISO)")
                         .arg(name)}};
        return {{QStringLiteral("ok"), true},
                {QStringLiteral("view"), name.toUpper()}};
    }
    if (method == QLatin1String("sketchface")) {
        beginSketchOnFace();
        return {{QStringLiteral("ok"), true}};
    }
    if (method == QLatin1String("insertstep")) {
        QString error;
        if (!insertStepFile(params[QStringLiteral("path")].toString(), error))
            return {{QStringLiteral("error"),
                     error.isEmpty() ? QStringLiteral("insert failed") : error}};
        return {{QStringLiteral("ok"), true}};
    }
    if (method == QLatin1String("screenshot")) {
        const QString path = params[QStringLiteral("path")].toString();
        if (path.isEmpty())
            return {{QStringLiteral("error"), QStringLiteral("path required")}};
        // Optional "overlays": false (CLI: `screenshot PATH clean`) captures
        // the 2D document WITHOUT decorations (grid, UCS icon, crosshair,
        // snap glyphs) — geometry-only, for image diffs against reference
        // renderers. By definition a 2D document render, so it applies even
        // while the 3D view is active (it must not be silently ignored).
        // Absent = historic behavior (active-view grab).
        if (!params[QStringLiteral("overlays")].toBool(true)) {
            if (!m_canvas->contentImage().save(path))
                return {{QStringLiteral("error"),
                         QStringLiteral("cannot write %1").arg(path)}};
            return {{QStringLiteral("ok"), true}, {QStringLiteral("savedTo"), path}};
        }
        // In 3D, dump the OCCT framebuffer (QWidget::grab can't capture a
        // native GL window). Otherwise grab the 2D canvas.
        const bool in3d = m_occtView && m_viewStack &&
                          m_viewStack->currentWidget() == m_occtView;
        if (in3d) {
            if (!m_occtView->dumpToFile(path))
                return {{QStringLiteral("error"),
                         QStringLiteral("3D dump failed")}};
            return {{QStringLiteral("ok"), true}, {QStringLiteral("savedTo"), path}};
        }
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
    if (!file.open(QIODevice::ReadOnly)) {
        m_commandBar->appendHistory(
            QStringLiteral("! shortcuts: cannot read %1 — custom shortcuts "
                           "not loaded")
                .arg(path));
        return;
    }
    // Rebuildable: Preferences edits drop the old bindings and re-run this.
    for (QShortcut* sc : m_userShortcuts)
        sc->deleteLater();
    m_userShortcuts.clear();
    // Keys already owned by menu actions: binding them AGAIN would make Qt
    // declare the shortcut ambiguous and fire NEITHER (the historic dead
    // Ctrl+Z). Skip them with a note instead.
    QSet<QString> reserved;
    const auto collect = [&reserved](const QList<QAction*>& actions) {
        for (const QAction* a : actions)
            for (const QKeySequence& k : a->shortcuts())
                if (!k.isEmpty())
                    reserved.insert(k.toString());
    };
    for (const QAction* menuAct : menuBar()->actions())
        if (menuAct->menu())
            collect(menuAct->menu()->actions());
    const QJsonObject map = QJsonDocument::fromJson(file.readAll()).object();
    for (auto it = map.begin(); it != map.end(); ++it) {
        const QString keyText = QKeySequence(it.key()).toString();
        if (reserved.contains(keyText)) {
            m_commandBar->appendHistory(
                QStringLiteral("shortcuts.json: %1 is a built-in menu "
                               "shortcut — entry ignored")
                    .arg(keyText));
            continue;
        }
        const QString commandLine = it.value().toString();
        auto* shortcut = new QShortcut(QKeySequence(it.key()), this);
        shortcut->setContext(Qt::ApplicationShortcut);
        connect(shortcut, &QShortcut::activated, this, [this, commandLine] {
            onCommandEntered(commandLine);
        });
        m_userShortcuts.push_back(shortcut);
    }
}

void MainWindow::loadMousePrefs()
{
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QFile file(dir + QStringLiteral("/prefs.json"));
    if (!file.open(QIODevice::ReadOnly))
        return; // defaults
    const QJsonObject o = QJsonDocument::fromJson(file.readAll()).object();
    if (o.contains(QStringLiteral("orbit")))
        m_mousePrefs.orbit = o[QStringLiteral("orbit")].toString();
    if (o.contains(QStringLiteral("pan")))
        m_mousePrefs.pan = o[QStringLiteral("pan")].toString();
    m_mousePrefs.zoomInvert = o[QStringLiteral("zoomInvert")].toBool(false);
}

void MainWindow::openPreferences()
{
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    // Current shortcuts, straight from the file the bindings were built from.
    QJsonObject shortcuts;
    {
        QFile file(dir + QStringLiteral("/shortcuts.json"));
        if (file.open(QIODevice::ReadOnly))
            shortcuts = QJsonDocument::fromJson(file.readAll()).object();
    }
    PreferencesDialog dialog(m_mousePrefs, shortcuts, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    // Persist + apply the mouse mapping.
    m_mousePrefs = dialog.mousePrefs();
    {
        QFile file(dir + QStringLiteral("/prefs.json"));
        if (file.open(QIODevice::WriteOnly)) {
            const QJsonObject o{
                {QStringLiteral("orbit"), m_mousePrefs.orbit},
                {QStringLiteral("pan"), m_mousePrefs.pan},
                {QStringLiteral("zoomInvert"), m_mousePrefs.zoomInvert}};
            file.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
        }
    }
    if (m_occtView)
        m_occtView->setMouseBindings(m_mousePrefs.orbitButton(),
                                     m_mousePrefs.panButton(),
                                     m_mousePrefs.zoomInvert);
    // Persist + rebuild the keyboard shortcuts.
    {
        QFile file(dir + QStringLiteral("/shortcuts.json"));
        if (file.open(QIODevice::WriteOnly))
            file.write(QJsonDocument(dialog.shortcuts())
                           .toJson(QJsonDocument::Indented));
    }
    loadShortcuts();
    m_commandBar->appendHistory(QStringLiteral("Preferences applied"));
}

MainWindow::~MainWindow() = default;

void MainWindow::adoptDocument(std::unique_ptr<Document> doc)
{
    m_selection.clear();
    m_doc = std::move(doc);
    m_ctx = std::make_unique<CommandContext>(*m_doc, m_selection, m_canvas);
    m_processor = std::make_unique<CommandProcessor>(*m_ctx);
    registerBuiltinCommands(*m_processor);
    m_commandBar->setCompletions(m_processor->commandNames());
    m_canvas->attach(m_doc.get(), m_processor.get(), &m_selection);
    if (m_occtView)
        m_occtView->attach(m_doc.get(), m_processor.get(), &m_selection);
    m_layerPanel->attach(m_doc.get());
    m_propsPanel->attach(m_doc.get(), &m_selection);
    m_assemblyPanel->attach(m_doc.get(), &m_selection);
    m_doc->addChangeListener([this] {
        m_docDirty3d = true; // the 3D scene needs a real rebuild on next sync
        m_layerPanel->refresh();
        m_assemblyPanel->refresh();
        updateSketchStatus(); // sketch open/close/rename fires this too
    });
    updateSketchStatus();
    m_canvas->clearSketchReference();
    m_canvas->setMirroredX(false); // BOARDVIEW BOTTOM mirror is per-document view state
    m_doc->clearExtraSnapPoints();
    m_canvas->zoomExtents();
    updateWindowTitle();
    updateUnitsButton();
    refreshPromptAndMessages();

    // A solids-only drawing (e.g. an imported STEP) is meaningless in the 2D
    // view — it shows only footprint placeholders. Jump straight to 3D.
    if (documentIsSolidsOnly()) {
        setView3D(true);
        m_commandBar->appendHistory(
            QStringLiteral("3D model — orbit: drag; pan: middle-drag; zoom: "
                           "wheel. Toggle the 3D button for the 2D view."));
    } else if (m_3dButton && m_3dButton->isChecked()) {
        setView3D(false); // returning to a 2D drawing
    }
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
    // Typed 3D commands (HOLE, SHELL, PATTERN3D…) must show up without having
    // to toggle the view off and on again — but only rebuild if they actually
    // changed the document.
    sync3DView();
    // Give the focus back to the drawing view: leaving it in the command bar
    // made its QLineEdit swallow Ctrl+Z as a TEXT undo (so "undo didn't
    // work"), and typing forwards to the bar from either view anyway.
    if (m_viewStack && m_viewStack->currentWidget())
        m_viewStack->currentWidget()->setFocus();
}

void MainWindow::sync3DView()
{
    if (!m_occtView || m_viewStack->currentWidget() != m_occtView)
        return;
    if (m_docDirty3d) {
        m_docDirty3d = false;
        m_occtView->refreshFrom(*m_doc);
    } else {
        m_occtView->syncHighlight();
    }
}

void MainWindow::onInteraction()
{
    refreshPromptAndMessages();
    m_propsPanel->refresh();
    // Mouse-driven commands mutate solids too (e.g. HOLE placed by a 3D
    // click): keep the 3D view in sync when it is the active view, and light
    // up the selected solid's row in the Assembly tree.
    if (m_occtView && m_viewStack->currentWidget() == m_occtView) {
        sync3DView();
        m_assemblyPanel->refresh();
    }
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

void MainWindow::updateSketchStatus()
{
    if (!m_sketchLabel || !m_doc)
        return;
    const SketchInfo* info = m_doc->sketchById(m_doc->activeSketch());
    m_sketchLabel->setVisible(info != nullptr);
    m_sketchLabel->setText(
        info ? QStringLiteral("Sketch '%1' — SKETCH Close to finish").arg(info->name)
             : QString());
    if (m_finishSketchBtn)
        m_finishSketchBtn->setVisible(info != nullptr);
    if (!info)
        m_sketchFrom3d = false; // closed some other way — drop the flag
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

bool MainWindow::loadFile(const QString& path, bool interactive)
{
    m_openWarnings.clear();
    const auto reportError = [&](const QString& msg) {
        if (interactive)
            QMessageBox::warning(this, QStringLiteral("Open failed"), msg);
        else
            m_commandBar->appendHistory(QStringLiteral("! %1").arg(msg));
        return false;
    };
    const auto ends = [&](const char* ext) {
        return path.endsWith(QLatin1String(ext), Qt::CaseInsensitive);
    };

    // A directory is a Gerber kit (fab outputs, one layer per file) — this is
    // also the headless path (IPC "open"), so gui-smoke can exercise kits.
    if (QFileInfo(path).isDir())
        return loadGerberKit(path, interactive);

    if (ends(".vkd")) {
        QString error;
        auto doc = NativeStore::load(path, error);
        if (!doc)
            return reportError(error);
        adoptDocument(std::move(doc));
        return true;
    }
#ifdef VIKICAD_HAS_DXF
    if (ends(".dxf") || ends(".dwg")) {
        DxfImportResult r = ends(".dwg") ? importDwg(path) : importDxf(path);
        if (!r.ok)
            return reportError(r.error);
        adoptDocument(std::move(r.document));
        QString msg = QStringLiteral("Imported %1 entities from %2")
                          .arg(r.imported)
                          .arg(path);
        if (r.imported == 0)
            msg = QStringLiteral("⚠ %1 opened but 0 entities imported (%2 "
                                 "skipped)").arg(path).arg(r.skipped);
        else if (r.skipped > 0)
            msg += QStringLiteral(" (%1 skipped: %2)")
                       .arg(r.skipped)
                       .arg(r.skippedTypes.join(QStringLiteral(", ")));
        m_commandBar->appendHistory(msg);
        return true;
    }
#endif
    if (ends(".step") || ends(".stp")) {
        std::unique_ptr<Document> doc;
        const StepResult r = importStep(path, doc);
        if (!r.ok || !doc)
            return reportError(r.error.isEmpty() ? QStringLiteral("STEP import failed")
                                                 : r.error);
        // Tag the solids as a named component (so it shows in the assembly).
        const QString comp = QFileInfo(path).completeBaseName();
        for (const EntityId id : doc->drawOrder())
            if (auto* s = dynamic_cast<SolidEntity*>(doc->entity(id)))
                s->component = comp;
        adoptDocument(std::move(doc));
        m_commandBar->appendHistory(
            QStringLiteral("Imported %1 solids from %2 (%3 notes)")
                .arg(r.solids).arg(path).arg(r.notes));
        return true;
    }
    // A lone Gerber/Excellon file (any extension — .GTL, .TXT drills, .pho…)
    // opens as a one-file kit, exactly like the CLI import path. Content
    // sniff, so it never hijacks the known extensions handled above.
    if (looksLikeGerberOrExcellon(path))
        return loadGerberKit(path, interactive);
    return reportError(QStringLiteral("unsupported file type: %1").arg(path));
}

void MainWindow::openPath(const QString& path)
{
    loadFile(path, /*interactive=*/false);
}

void MainWindow::newFile()
{
    adoptDocument(std::make_unique<Document>());
}

void MainWindow::openFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open drawing"), {},
        QStringLiteral("All supported (*.vkd *.dxf *.dwg *.step *.stp);;"
                       "VikiCAD drawings (*.vkd);;"
                       "DXF/DWG (*.dxf *.dwg);;"
                       "STEP (*.step *.stp);;"
                       "All files (*)"));
    if (path.isEmpty())
        return;
    loadFile(path, /*interactive=*/true);
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

void MainWindow::openGerberKit()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Open Gerber kit (fabrication output directory)"));
    if (dir.isEmpty())
        return;
    loadGerberKit(dir, /*interactive=*/true);
}

bool MainWindow::loadGerberKit(const QString& path, bool interactive)
{
    auto doc = std::make_unique<Document>();
    const GerberKitResult r = importGerberKit(*doc, path);
    if (!r.ok) {
        if (interactive)
            QMessageBox::warning(this, QStringLiteral("Gerber kit import failed"),
                                 r.error);
        else
            m_commandBar->appendHistory(QStringLiteral("! %1").arg(r.error));
        return false;
    }
    // The kit was imported as ONE transaction into the fresh document, so a
    // single Ctrl+Z after adoption restores an empty drawing.
    adoptDocument(std::move(doc));
    m_commandBar->appendHistory(
        QStringLiteral("Gerber kit: %1 file(s) -> %2 layer(s), %3 entities from %4")
            .arg(r.files.size())
            .arg(r.layers.size())
            .arg(r.entities)
            .arg(path));
    if (!r.skipped.isEmpty())
        m_commandBar->appendHistory(
            QStringLiteral("  skipped: %1").arg(r.skipped.join(QStringLiteral("; "))));
    if (!r.warnings.isEmpty())
        m_commandBar->appendHistory(
            QStringLiteral("  %1 warning(s), first: %2")
                .arg(r.warnings.size())
                .arg(r.warnings.first()));
    m_openWarnings = r.warnings; // surfaced by the IPC "open" reply
    return true;
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

void MainWindow::beginSketchOnFace()
{
    if (!m_doc || !m_occtView)
        return;
    const auto wp = solidops::planeFromFace(m_occtView->pickedFace());
    if (!wp) {
        m_commandBar->appendHistory(QStringLiteral("! sketch: pick a FLAT face"));
        return;
    }
    m_sketchFrom3d = true; // "Finish sketch" returns to the 3D view
    documentWorkplane(*m_doc) = *wp;
    // Project the real face outline into the sketch plane so the canvas shows
    // the face (holes and all), not a blank box.
    m_canvas->setSketchReference(
        solidops::faceOutline2d(m_occtView->pickedFace(), *wp));
    // Feed the face's vertices + arc/circle centers to the snap engine so the
    // profile can snap to real face features (see Document::extraSnapPoints).
    m_doc->setExtraSnapPoints(
        solidops::faceSnapPoints2d(m_occtView->pickedFace(), *wp));
    setView3D(false); // back to the 2D canvas to draw the profile
    // Face sketches are always captured as first-class sketches: auto-create
    // one on the plane just set, with a generated unique name, through the
    // shared processor (so CLI/IPC see the exact same state).
    {
        if (m_processor->hasActiveCommand())
            m_processor->cancelActive(); // the line below must start SKETCH
        // ONE token only: a multi-word name was split by the tokenizer, so
        // every face sketch ended up named "Sketch" and the SECOND one failed
        // silently — no active sketch, no isolation, untagged entities (the
        // "misalignment that would not die").
        QString name = QStringLiteral("FaceSketch-%1")
                           .arg(qlonglong(m_occtView->pickedSolid()));
        for (int n = 2; m_doc->sketchByName(name); ++n)
            name = QStringLiteral("FaceSketch-%1-%2")
                       .arg(qlonglong(m_occtView->pickedSolid()))
                       .arg(n);
        const auto r = m_processor->submit(
            QStringLiteral("SKETCH NEW %1").arg(name), /*strict=*/false);
        if (!r.ok)
            m_commandBar->appendHistory(QStringLiteral("! %1").arg(r.error));
        refreshPromptAndMessages();
        if (m_doc->activeSketch() == 0) {
            // Never enter a half-broken sketch mode: bail out visibly.
            m_canvas->clearSketchReference();
            m_doc->clearExtraSnapPoints();
            m_sketchFrom3d = false;
            m_commandBar->appendHistory(QStringLiteral(
                "! sketch on face ABORTED (could not open a sketch — see "
                "the message above)"));
            setView3D(true);
            return;
        }
    }
    m_commandBar->appendHistory(QStringLiteral(
        "Sketching on the face (blue dashed outline). Draw a closed profile, "
        "then EXTRUDE. SKETCH Close to finish; WORKPLANE XY resets."));
}

void MainWindow::insertStepComponent()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Insert STEP as component"), {},
        QStringLiteral("STEP (*.step *.stp);;All files (*)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!insertStepFile(path, error))
        QMessageBox::warning(this, QStringLiteral("Insert failed"),
                             error.isEmpty() ? QStringLiteral("STEP import failed")
                                             : error);
}

namespace {
// Fab extensions the export dispatch recognizes (lone-layer Gerber/Excellon).
bool isFabSuffix(const QString& suffix)
{
    static const QStringList kFab{
        QStringLiteral("gtl"), QStringLiteral("gbl"), QStringLiteral("gts"),
        QStringLiteral("gbs"), QStringLiteral("gto"), QStringLiteral("gbo"),
        QStringLiteral("gtp"), QStringLiteral("gbp"), QStringLiteral("gko"),
        QStringLiteral("gbr"), QStringLiteral("ger"), QStringLiteral("txt"),
        QStringLiteral("drl")};
    return kFab.contains(suffix);
}
} // namespace

QString MainWindow::docBaseName() const
{
    const QString base =
        m_doc ? QFileInfo(m_doc->filePath()).completeBaseName() : QString();
    return base.isEmpty() ? QStringLiteral("board") : base;
}

bool MainWindow::exportToPath(const QString& path, QString& message,
                              const QString& layer, QStringList* warnings)
{
    const auto reportWarnings = [this, warnings](const QStringList& ws) {
        for (const QString& w : ws)
            m_commandBar->appendHistory(QStringLiteral("! export: %1").arg(w));
        if (warnings)
            *warnings += ws;
    };

    // Gerber kit: a directory target writes <docname>.GTL/... + .TXT drills.
    if (QFileInfo(path).isDir() || path.endsWith(QLatin1Char('/'))) {
        const GerberKitExportResult r =
            exportGerberKit(*m_doc, path, docBaseName());
        if (!r.ok) {
            message = r.error;
            return false;
        }
        reportWarnings(r.warnings);
        for (const QString& s : r.skippedLayers)
            m_commandBar->appendHistory(
                QStringLiteral("export kit: skipped %1").arg(s));
        QStringList names;
        for (const GerberKitExportFile& f : r.files)
            names << QFileInfo(f.path).fileName();
        message = QStringLiteral("Gerber kit: %1 file(s) → %2 (%3)")
                      .arg(r.files.size())
                      .arg(path, names.join(QStringLiteral(", ")));
        return true;
    }

    const QString suffix = QFileInfo(path).suffix().toLower();
    if (isFabSuffix(suffix)) {
        QString layerName = layer;
        if (layerName.isEmpty()) {
            const QStringList candidates = layersForKitExtension(*m_doc, suffix);
            if (candidates.isEmpty()) {
                message = QStringLiteral(
                              "no layer matches .%1 — name the source layer")
                              .arg(suffix);
                return false;
            }
            if (candidates.size() > 1 && (suffix == QLatin1String("txt") ||
                                          suffix == QLatin1String("drl"))) {
                // Several drill layers group into ONE Excellon file.
                const ExcellonExportResult r =
                    exportExcellon(*m_doc, candidates, path);
                if (!r.ok) {
                    message = r.error;
                    return false;
                }
                reportWarnings(r.warnings);
                message = QStringLiteral(
                              "Excellon: %1 hole(s), %2 tool(s) [%3] → %4")
                              .arg(r.holes)
                              .arg(r.tools)
                              .arg(candidates.join(QStringLiteral("+")), path);
                return true;
            }
            if (candidates.size() > 1) {
                message = QStringLiteral("ambiguous .%1 (%2) — name the "
                                         "source layer")
                              .arg(suffix, candidates.join(QStringLiteral(", ")));
                return false;
            }
            layerName = candidates.first();
        }
        const GerberKitExportResult r = exportFabLayer(*m_doc, layerName, path);
        if (!r.ok) {
            message = r.error;
            return false;
        }
        reportWarnings(r.warnings);
        const GerberKitExportFile& f = r.files.front();
        message = f.isDrill
                      ? QStringLiteral("Excellon: %1 hole(s) [%2] → %3")
                            .arg(f.entities)
                            .arg(layerName, path)
                      : QStringLiteral("Gerber: %1 entit%2 [%3] → %4")
                            .arg(f.entities)
                            .arg(f.entities == 1 ? QStringLiteral("y")
                                                 : QStringLiteral("ies"))
                            .arg(layerName, path);
        return true;
    }
    if (suffix == QLatin1String("step") || suffix == QLatin1String("stp")) {
        const StepResult r = exportStep(*m_doc, path);
        message = r.ok ? QStringLiteral("STEP: %1 solid(s), %2 note(s) → %3")
                             .arg(r.solids).arg(r.notes).arg(path)
                       : r.error;
        return r.ok;
    }
#ifdef VIKICAD_HAS_DXF
    if (suffix == QLatin1String("dxf")) {
        const DxfExportResult r = exportDxf(*m_doc, path);
        message = r.ok ? QStringLiteral("DXF: %1 entit%2 exported%3 → %4")
                             .arg(r.exported)
                             .arg(r.exported == 1 ? QStringLiteral("y")
                                                  : QStringLiteral("ies"))
                             .arg(r.skipped
                                      ? QStringLiteral(", %1 skipped (%2)")
                                            .arg(r.skipped)
                                            .arg(r.skippedTypes.join(
                                                QStringLiteral(", ")))
                                      : QString())
                             .arg(path)
                       : r.error;
        return r.ok;
    }
#endif
    if (suffix == QLatin1String("stl")) {
        const StlResult r = exportStl(*m_doc, path);
        message = r.ok ? QStringLiteral("STL: %1 solid(s) meshed → %2")
                             .arg(r.solids).arg(path)
                       : r.error;
        return r.ok;
    }
    if (suffix == QLatin1String("obj")) {
        const ObjResult r = exportObj(*m_doc, path);
        message = r.ok ? QStringLiteral("OBJ: %1 solid(s), %2 vertices, %3 "
                                        "faces → %4")
                             .arg(r.solids).arg(r.vertices).arg(r.faces)
                             .arg(path)
                       : r.error;
        return r.ok;
    }
    message = QStringLiteral("unsupported export format: .%1 "
                             "(step/stp, dxf, stl, obj)").arg(suffix);
    return false;
}

void MainWindow::exportAs(const QString& kind)
{
    QString filter;
    if (kind == QLatin1String("step"))
        filter = QStringLiteral("STEP (*.step *.stp)");
    else if (kind == QLatin1String("dxf"))
        filter = QStringLiteral("DXF drawing (*.dxf)");
    else if (kind == QLatin1String("stl"))
        filter = QStringLiteral("STL mesh (*.stl)");
    else
        filter = QStringLiteral("Wavefront OBJ (*.obj)");
    QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export %1").arg(kind.toUpper()), QString(),
        filter);
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QLatin1Char('.') + kind;
    QString message;
    if (exportToPath(path, message)) {
        m_commandBar->appendHistory(message);
        statusBar()->showMessage(message, 4000);
    } else {
        m_commandBar->appendHistory(QStringLiteral("! export: %1").arg(message));
        QMessageBox::warning(this, QStringLiteral("Export failed"), message);
    }
}

void MainWindow::exportGerberKitDir()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Export Gerber kit — choose a directory"));
    if (dir.isEmpty())
        return;
    QString message;
    if (exportToPath(dir, message)) {
        m_commandBar->appendHistory(message);
        statusBar()->showMessage(message, 4000);
    } else {
        m_commandBar->appendHistory(QStringLiteral("! export: %1").arg(message));
        QMessageBox::warning(this, QStringLiteral("Export failed"), message);
    }
}

void MainWindow::exportGerberLayerFile()
{
    // Fab-mapped layers first (with their kit extension), then the rest.
    QStringList items;
    for (const Layer& l : m_doc->layers()) {
        const QString ext = kitExtensionForLayer(l);
        if (!ext.isEmpty())
            items << QStringLiteral("%1 (%2)").arg(l.name, ext);
    }
    for (const Layer& l : m_doc->layers())
        if (kitExtensionForLayer(l).isEmpty())
            items << l.name;
    if (items.isEmpty())
        return;
    bool okd = false;
    const QString item = QInputDialog::getItem(
        this, QStringLiteral("Export Gerber/Excellon layer"),
        QStringLiteral("Layer:"), items, 0, false, &okd);
    if (!okd || item.isEmpty())
        return;
    const QString layerName = item.section(QStringLiteral(" ("), 0, 0);
    const Layer* layer = m_doc->layerByName(layerName);
    if (!layer)
        return;
    QString ext = kitExtensionForLayer(*layer);
    if (ext.isEmpty())
        ext = QStringLiteral(".GBR");
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export layer %1").arg(layerName),
        docBaseName() + ext,
        QStringLiteral("Fabrication files (*.gtl *.gbl *.gts *.gbs *.gto "
                       "*.gbo *.gtp *.gbp *.gko *.gbr *.txt *.drl);;"
                       "All files (*)"));
    if (path.isEmpty())
        return;
    QString message;
    if (exportToPath(path, message, layerName)) {
        m_commandBar->appendHistory(message);
        statusBar()->showMessage(message, 4000);
    } else {
        m_commandBar->appendHistory(QStringLiteral("! export: %1").arg(message));
        QMessageBox::warning(this, QStringLiteral("Export failed"), message);
    }
}

bool MainWindow::insertStepFile(const QString& path, QString& error)
{
    if (!m_doc)
        return false;
    std::unique_ptr<Document> tmp;
    const StepResult r = importStep(path, tmp);
    if (!r.ok || !tmp) {
        error = r.error;
        return false;
    }
    // Component name = the file's base name; each imported solid gets tagged.
    const QString comp = QFileInfo(path).completeBaseName();
    int added = 0;
    TransactionScope scope(*m_doc, QStringLiteral("INSERT STEP"));
    try {
        for (const EntityId id : tmp->drawOrder()) {
            const Entity* e = tmp->entity(id);
            if (!e)
                continue;
            auto copy = e->clone();
            if (auto* solid = dynamic_cast<SolidEntity*>(copy.get()))
                solid->component = comp;
            m_doc->addEntity(std::move(copy));
            ++added;
        }
        scope.commit();
    } catch (const std::exception& ex) {
        scope.rollback();
        error = QString::fromUtf8(ex.what());
        return false;
    }
    m_commandBar->appendHistory(
        QStringLiteral("Inserted component '%1' (%2 solid%3)")
            .arg(comp).arg(added).arg(added == 1 ? QString() : QStringLiteral("s")));
    // Sync the 3D view exactly once (a double refresh crashed some GL drivers).
    const bool already3d = m_3dButton && m_3dButton->isChecked();
    if (documentIsSolidsOnly() && !already3d)
        setView3D(true); // switches AND refreshes
    else if (m_occtView && m_viewStack->currentWidget() == m_occtView)
        m_occtView->refreshFrom(*m_doc);
    m_canvas->markDocumentDirty();
    if (m_assemblyPanel)
        m_assemblyPanel->refresh();
    return true;
}

void MainWindow::editEntity(EntityId id)
{
    if (!m_doc)
        return;
    Entity* probe = m_doc->entity(id);
    if (!probe) {
        m_commandBar->appendHistory(QStringLiteral(
            "! edit: entity %1 no longer exists").arg(qlonglong(id)));
        return;
    }
    // Only text has a bespoke editor; other types fall back to the panel.
    QJsonObject full = probe->toJson();
    if (full[QStringLiteral("type")].toString() != QLatin1String("text")) {
        m_selection.clear();
        m_selection.toggle(id);
        onInteraction();
        return;
    }
    QJsonObject geom = full[QStringLiteral("geom")].toObject();

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Edit text"));
    auto* lay = new QVBoxLayout(&dlg);
    auto* editor = new QPlainTextEdit(geom[QStringLiteral("text")].toString(), &dlg);
    editor->setMinimumSize(360, 160);
    lay->addWidget(editor);
    auto* form = new QFormLayout;
    auto* height = new QDoubleSpinBox(&dlg);
    height->setRange(0.001, 1e7);
    height->setDecimals(3);
    height->setValue(geom[QStringLiteral("height")].toDouble(3.5));
    auto* hAlign = new QComboBox(&dlg);
    hAlign->addItems({QStringLiteral("Left"), QStringLiteral("Center"),
                      QStringLiteral("Right")});
    hAlign->setCurrentIndex(geom[QStringLiteral("halign")].toInt(0));
    auto* vAlign = new QComboBox(&dlg);
    vAlign->addItems({QStringLiteral("Baseline"), QStringLiteral("Top"),
                      QStringLiteral("Middle"), QStringLiteral("Bottom")});
    vAlign->setCurrentIndex(geom[QStringLiteral("valign")].toInt(0));
    form->addRow(QStringLiteral("Height"), height);
    form->addRow(QStringLiteral("H align"), hAlign);
    form->addRow(QStringLiteral("V align"), vAlign);
    lay->addLayout(form);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    lay->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    editor->setFocus();
    if (dlg.exec() != QDialog::Accepted)
        return;

    geom[QStringLiteral("text")] = editor->toPlainText();
    geom[QStringLiteral("height")] = height->value();
    geom[QStringLiteral("halign")] = hAlign->currentIndex();
    geom[QStringLiteral("valign")] = vAlign->currentIndex();
    full[QStringLiteral("geom")] = geom;

    m_doc->beginTransaction(QStringLiteral("TEXTEDIT"));
    if (Entity* e = m_doc->beginModify(id)) {
        e->fromJson(full);
        m_doc->endModify(id);
    }
    m_doc->commitTransaction();
    onInteraction();
}

} // namespace viki
