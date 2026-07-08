#pragma once

#include <memory>

#include <QMainWindow>

#include "cmd/CommandProcessor.h"
#include "doc/Document.h"
#include "doc/SelectionSet.h"

class QLabel;
class QStackedWidget;
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
    RpcServer* m_rpc = nullptr;
    OcctViewWidget* m_occtView = nullptr;
    QStackedWidget* m_viewStack = nullptr;
    QString m_lastCommand;

    void toggle3D(bool on);
    void setView3D(bool on);  // programmatic switch (keeps the button in sync)
    bool documentIsSolidsOnly() const;
    QJsonObject handleRpc(const QString& method, const QJsonObject& params);
    void loadShortcuts();
};

} // namespace viki
