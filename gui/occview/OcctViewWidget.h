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

class CommandProcessor;

// OCCT AIS 3D view (shaded solids). Rotate = left drag, pan = middle drag,
// zoom = wheel. Rebuilt from the document on every activation.
//
// When a command is running and asks for a Point, the 3D view becomes an input
// device (Fusion-style): hovering a planar face makes it the active work
// plane, the cursor position on it feeds the command's pointer hint and ghost
// preview (red = material removed, blue = added), and a click supplies the
// point — plus the face's owning solid if the command then asks for one.
class OcctViewWidget : public QWidget {
    Q_OBJECT
public:
    explicit OcctViewWidget(QWidget* parent = nullptr);

    // Wire the document + command processor (same pattern as CanvasWidget).
    void attach(Document* doc, CommandProcessor* processor);

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
    // Right-click "Sketch on this face": set the work plane to the picked face.
    void sketchOnFace();
    // Right-click "Split solid by this face's plane": split the owning solid
    // (pickedSolid) by the plane of the picked face.
    void splitByFace();
    // Command input was provided from the 3D view (same contract as
    // CanvasWidget::interaction): the main window refreshes prompt + views.
    void interaction();

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

    // Command-input pipeline (active-command Point prompts only).
    bool commandWantsPoint() const;
    // Resolve the cursor into work-plane 2D coords: prefer the surface point of
    // the planar face under the cursor (which also becomes the work plane and
    // snaps to the face's feature points); fall back to a ray/work-plane
    // intersection in free space. Updates m_hover* members.
    bool cursorToPlane(const QPoint& physical, Vec2d& uv);
    void updateGhost(const Vec2d& uv);
    void clearGhost();

    Handle(V3d_Viewer) m_viewer;
    Handle(V3d_View) m_view;
    Handle(AIS_InteractiveContext) m_context;
    QPoint m_lastPos;
    QPoint m_pressPos;
    bool m_initFailed = false;
    bool m_fittedOnce = false;

    Document* m_doc = nullptr;
    CommandProcessor* m_processor = nullptr;

    // Map each displayed shape back to its document entity, and remember the
    // last picked face + its owning solid (for Push/Pull).
    std::vector<std::pair<Handle(AIS_InteractiveObject), EntityId>> m_shapes;
    TopoDS_Shape m_pickedFace;
    EntityId m_pickedSolid = kInvalidEntityId;

    // Transient ghost of the active command's pending result.
    Handle(AIS_InteractiveObject) m_ghost;
    // What the cursor last resolved to during a Point prompt.
    bool m_hoverValid = false;
    Vec2d m_hoverUv;
    EntityId m_hoverSolid = kInvalidEntityId;
};

} // namespace viki
