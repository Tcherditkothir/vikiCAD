#include "CanvasWidget.h"

#include <QMouseEvent>
#include <QPainter>

namespace viki {

CanvasWidget::CanvasWidget(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setCursor(Qt::BlankCursor);
}

void CanvasWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(24, 26, 28));

    p.setPen(QColor(220, 220, 220));
    p.drawLine(0, m_cursor.y(), width(), m_cursor.y());
    p.drawLine(m_cursor.x(), 0, m_cursor.x(), height());
}

void CanvasWidget::mouseMoveEvent(QMouseEvent* event)
{
    m_cursor = event->pos();
    update();
}

} // namespace viki
