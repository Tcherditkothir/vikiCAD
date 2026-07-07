#pragma once

#include <QWidget>

namespace viki {

// M0 placeholder: dark drafting background + crosshair. The static-pixmap +
// overlay pipeline lands in M1.
class CanvasWidget : public QWidget {
    Q_OBJECT
public:
    explicit CanvasWidget(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    QPoint m_cursor;
};

} // namespace viki
