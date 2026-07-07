#pragma once

#include <QMainWindow>

namespace viki {

class CanvasWidget;
class CommandBar;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();

private slots:
    void onCommandEntered(const QString& line);

private:
    CanvasWidget* m_canvas = nullptr;
    CommandBar* m_commandBar = nullptr;
};

} // namespace viki
