#pragma once

#include <optional>

#include <QPixmap>
#include <QWidget>

#include "Camera2d.h"
#include "cmd/CommandProcessor.h"
#include "snap/SnapEngine.h"

namespace viki {

// The 2D drafting viewport: static pixmap of committed entities + dynamic
// overlay (crosshair, command preview, selection, rubber band, snap glyph).
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
    void zoomWindow(const BBox2d& box) override;

    // Input-mode toggles (status bar buttons).
    SnapSettings& snapSettings() { return m_snapSettings; }
    void setOrtho(bool on) { m_ortho = on; }
    void setPolar(bool on) { m_polar = on; }
    void setGridSnap(bool on) { m_gridSnap = on; markDocumentDirty(); }
    bool ortho() const { return m_ortho; }
    bool polar() const { return m_polar; }
    bool gridSnap() const { return m_gridSnap; }

signals:
    // Emitted after any input reached the command processor or the selection
    // changed — MainWindow refreshes the prompt/history from this.
    void interaction();
    // World-coordinates cursor position for the status readout.
    void cursorMoved(double x, double y);
    // Printable key typed on the canvas — route to the command bar
    // (the AutoCAD "just start typing" workflow).
    void typed(const QString& text);

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
    void drawGrid(QPainter& painter) const;
    void drawPrimitives(QPainter& painter, const PrimitiveList& list,
                        const QColor& overrideColor = QColor()) const;
    void drawSnapGlyph(QPainter& painter) const;
    void handleLeftClick(const Vec2d& world, Qt::KeyboardModifiers mods);
    void finishRubberBand(const QPointF& releasePos, Qt::KeyboardModifiers mods);
    // Object snap → ortho/polar → grid, in that order.
    Vec2d effectivePoint(const Vec2d& cursorWorld);
    bool commandWantsPoint() const;
    double pickTolerance() const { return m_camera.pixelsToWorld(6.0); }

    Document* m_doc = nullptr;
    CommandProcessor* m_processor = nullptr;
    SelectionSet* m_selection = nullptr;

    Camera2d m_camera;
    QPixmap m_staticLayer;
    bool m_staticDirty = true;

    SnapSettings m_snapSettings;
    bool m_ortho = false;
    bool m_polar = false;
    bool m_gridSnap = false;
    double m_gridSpacing = 10.0; // mm
    std::optional<SnapResult> m_activeSnap;
    Vec2d m_effectiveCursor;

    // Grip editing (selection, no active command).
    struct GripDrag {
        bool active = false;
        EntityId id = kInvalidEntityId;
        int index = -1;
    };
    GripDrag m_grip;
    std::optional<std::pair<EntityId, int>> gripAt(const QPointF& px) const;
    void drawGrips(QPainter& p) const;

    QPointF m_cursorPx;
    bool m_panning = false;
    QPointF m_panLast;
    bool m_rubberBand = false;
    QPointF m_rubberStart;
};

} // namespace viki
