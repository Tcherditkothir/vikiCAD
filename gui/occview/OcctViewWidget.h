#pragma once

#include <utility>
#include <vector>

#include <QWidget>

#include <AIS_InteractiveContext.hxx>
#include <AIS_InteractiveObject.hxx>
#include <TopoDS_Shape.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>

#include "doc/Document.h"

namespace viki {

// OCCT AIS 3D view (shaded solids). Rotate = left drag, pan = middle drag,
// zoom = wheel. Rebuilt from the document on every activation.
class OcctViewWidget : public QWidget {
    Q_OBJECT
public:
    explicit OcctViewWidget(QWidget* parent = nullptr);

    // (Re)display every solid in the document and fit the view.
    void refreshFrom(const Document& doc);
    bool isReady() const { return !m_view.IsNull(); }
    // Dump the 3D framebuffer to an image file (QWidget::grab can't capture
    // OCCT's native GL window). Used by the screenshot IPC in 3D mode.
    bool dumpToFile(const QString& path);

    // The face last clicked (empty if none / a whole solid was picked).
    const TopoDS_Shape& pickedFace() const { return m_pickedFace; }
    EntityId pickedSolid() const { return m_pickedSolid; }
    // Select at physical-pixel coords (shared by the mouse and the pick3d IPC
    // verb); returns a human-readable description of what got selected.
    QString pickAtPhysical(int px, int py);
    // Pick at the physical centre of the view (for headless verification).
    QString pickCenter();

signals:
    // Emitted on pick: a short human-readable description of what's selected.
    void picked(const QString& info);
    // Right-click "Push/Pull" on a selected face: extrude it by `distance`.
    void pushPullFace(EntityId solid, double distance);

protected:
    void contextMenuEvent(QContextMenuEvent* event) override;
    QPaintEngine* paintEngine() const override { return nullptr; }
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void initViewer();
    // Logical (Qt) -> physical (OCCT) pixel mapping for HiDPI displays.
    QPoint devicePos(const QPoint& logical) const;

    Handle(V3d_Viewer) m_viewer;
    Handle(V3d_View) m_view;
    Handle(AIS_InteractiveContext) m_context;
    QPoint m_lastPos;
    QPoint m_pressPos;
    bool m_initFailed = false;

    // Map each displayed shape back to its document entity, and remember the
    // last picked face + its owning solid (for Push/Pull).
    std::vector<std::pair<Handle(AIS_InteractiveObject), EntityId>> m_shapes;
    TopoDS_Shape m_pickedFace;
    EntityId m_pickedSolid = kInvalidEntityId;
};

} // namespace viki
