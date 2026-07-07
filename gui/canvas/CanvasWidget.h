#pragma once

#include <functional>

#include <QPixmap>
#include <QWidget>

#include "Camera2d.h"
#include "cmd/CommandProcessor.h"

namespace viki {

// The 2D drafting viewport: static pixmap of committed entities + dynamic
// overlay (crosshair, command preview, selection, rubber band) per frame.
// Also the GUI's ViewHook (ZOOM Extents etc.).
class CanvasWidget : public QWidget, public ViewHook {
    Q_OBJECT
public:
    explicit CanvasWidget(QWidget* parent = nullptr);

    // MainWindow wires (and re-wires on file open) the working set.
    void attach(Document* doc, CommandProcessor* processor, SelectionSet* selection);
    void markDocumentDirty();

    // ViewHook
    void zoomExtents() override;

signals:
    // Emitted after any input reached the command processor or the selection
    // changed — MainWindow refreshes the prompt/history from this.
    void interaction();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void rebuildStaticLayer();
    void drawPrimitives(QPainter& painter, const PrimitiveList& list,
                        const QColor& overrideColor = QColor()) const;
    void handleLeftClick(const Vec2d& world, Qt::KeyboardModifiers mods);
    void finishRubberBand(const QPointF& releasePos, Qt::KeyboardModifiers mods);
    double pickTolerance() const { return m_camera.pixelsToWorld(6.0); }

    Document* m_doc = nullptr;
    CommandProcessor* m_processor = nullptr;
    SelectionSet* m_selection = nullptr;

    Camera2d m_camera;
    QPixmap m_staticLayer;
    bool m_staticDirty = true;

    QPointF m_cursorPx;
    bool m_panning = false;
    QPointF m_panLast;
    bool m_rubberBand = false;
    QPointF m_rubberStart;
};

} // namespace viki
