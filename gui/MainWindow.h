#pragma once

#include <memory>
#include <vector>

#include <QMainWindow>

#include "PreferencesDialog.h" // MousePrefs
#include "cmd/CommandProcessor.h"
#include "doc/Document.h"
#include "doc/SelectionSet.h"

class QLabel;
class QShortcut;
class QStackedWidget;
class QTabWidget;
class QToolBar;
class QToolButton;

namespace viki {

class CanvasWidget;
class CommandBar;
class OcctViewWidget;
class LayerPanel;
class PropertiesPanel;
class AssemblyPanel;
class RpcServer;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

    // Opens any supported file (used for command-line / desktop launch).
    void openPath(const QString& path);

private slots:
    void onCommandEntered(const QString& line);
    void onInteraction();
    void newFile();
    void openFile();
    void saveFile();
    void saveFileAs();
    void importDxfFile();
    void insertStepComponent(); // additive STEP import into the current doc (menu)
    void toggleUnits();

private:
    // Load any supported drawing by extension (.vkd/.dxf/.dwg/.step). On
    // failure, shows a dialog when interactive, else logs to the command bar.
    bool loadFile(const QString& path, bool interactive);
    bool insertStepFile(const QString& path, QString& error); // additive, shared
    void beginSketchOnFace(); // set the work plane to the picked 3D face
    void editEntity(EntityId id); // double-click editor (text)
    void adoptDocument(std::unique_ptr<Document> doc);
    void refreshPromptAndMessages();
    void updateWindowTitle();
    void updateUnitsButton();
    bool saveTo(const QString& path);

    std::unique_ptr<Document> m_doc;
    SelectionSet m_selection;
    std::unique_ptr<CommandContext> m_ctx;
    std::unique_ptr<CommandProcessor> m_processor;

    CanvasWidget* m_canvas = nullptr;
    CommandBar* m_commandBar = nullptr;
    LayerPanel* m_layerPanel = nullptr;
    PropertiesPanel* m_propsPanel = nullptr;
    AssemblyPanel* m_assemblyPanel = nullptr;
    QLabel* m_coordLabel = nullptr;
    QToolButton* m_unitsBtn = nullptr;
    QToolButton* m_3dButton = nullptr;
    QTabWidget* m_toolTabs = nullptr; // Draw/…/Solids/Views tool tab strip
    int m_viewsTabIndex = -1;         // Views tab: enabled in 3D mode only
    RpcServer* m_rpc = nullptr;
    OcctViewWidget* m_occtView = nullptr;
    QStackedWidget* m_viewStack = nullptr;
    QString m_lastCommand;

    void toggle3D(bool on);
    void setView3D(bool on);  // programmatic switch (keeps the button in sync)
    // Bring the 3D view up to date CHEAPLY: full scene rebuild only when the
    // document changed since the last sync (m_docDirty3d, set by the change
    // listener); a mere selection change just re-applies the highlight.
    // Rebuilding on every click recomputes every shape's selection structures
    // and froze the GUI on real STEP parts.
    void sync3DView();
    bool m_docDirty3d = false;
    bool documentIsSolidsOnly() const;
    QJsonObject handleRpc(const QString& method, const QJsonObject& params);
    void loadShortcuts();
    // Preferences: mouse mapping (prefs.json) + editable shortcut table
    // (shortcuts.json). openPreferences() re-applies both live.
    void openPreferences();
    void loadMousePrefs();
    MousePrefs m_mousePrefs;
    std::vector<QShortcut*> m_userShortcuts; // rebuilt when edited
};

} // namespace viki
