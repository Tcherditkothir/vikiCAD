#pragma once

#include <memory>

#include <QMainWindow>

#include "cmd/CommandProcessor.h"
#include "doc/Document.h"
#include "doc/SelectionSet.h"

class QLabel;
class QToolButton;

namespace viki {

class CanvasWidget;
class CommandBar;
class LayerPanel;
class PropertiesPanel;
class RpcServer;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

private slots:
    void onCommandEntered(const QString& line);
    void onInteraction();
    void newFile();
    void openFile();
    void saveFile();
    void saveFileAs();
    void importDxfFile();
    void toggleUnits();

private:
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
    QLabel* m_coordLabel = nullptr;
    QToolButton* m_unitsBtn = nullptr;
    RpcServer* m_rpc = nullptr;

    QJsonObject handleRpc(const QString& method, const QJsonObject& params);
    void loadShortcuts();
};

} // namespace viki
