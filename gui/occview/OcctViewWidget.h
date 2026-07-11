#pragma once

#include <utility>
#include <vector>

#include <QWidget>

#include <AIS_InteractiveContext.hxx>
#include <AIS_InteractiveObject.hxx>
#include <AIS_ViewCube.hxx>
#include <TopoDS_Shape.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>

#include "doc/Document.h"
#include "doc/SelectionSet.h"

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

    // Wire the document + command processor + shared selection (same pattern
    // as CanvasWidget). Clicking a solid selects it; refreshFrom() highlights
    // whatever the document selection holds.
    void attach(Document* doc, CommandProcessor* processor, SelectionSet* selection);

    // (Re)display every solid in the document and fit the view. EXPENSIVE on
    // real parts (recomputes every shape's selection structures) — call it
    // when the DOCUMENT changed, never for a mere selection change.
    void refreshFrom(const Document& doc);
    // Cheap counterpart: re-apply the orange highlight from the document
    // selection onto the already-displayed shapes. Call on selection changes.
    void syncHighlight();

    // Standard view orientation (TOP/FRONT/RIGHT/ISO… — views::standardViewDir
    // names); frames the scene. Returns false for an unknown name / no view.
    bool setStandardView(const QString& name);
    // Frame everything (the toolbar "Fit" button).
    void fitView();
    // 10 mm reference grid in the 3D view, on/off.
    void setGridVisible(bool on);
    // Mouse mapping (Preferences): which button orbits / pans, zoom wheel
    // direction. Buttons may be Qt::LeftButton/MiddleButton/RightButton.
    void setMouseBindings(Qt::MouseButton orbit, Qt::MouseButton pan,
                          bool zoomInvert);
    bool isReady() const { return !m_view.IsNull(); }
    // Dump the 3D framebuffer to an image file (QWidget::grab can't capture
    // OCCT's native GL window). Used by the screenshot IPC in 3D mode.
    bool dumpToFile(const QString& path);

    // The face last clicked (empty if none / a whole solid was picked).
    const TopoDS_Shape& pickedFace() const { return m_pickedFace; }
    EntityId pickedSolid() const { return m_pickedSolid; }
    // EVERY sub-shape currently picked on the picked solid (Ctrl+click
    // accumulates): feeds fillet/chamfer (edges) and shell-open (faces).
    const std::vector<TopoDS_Shape>& pickedFaces() const { return m_pickedFaces; }
    const std::vector<TopoDS_Shape>& pickedEdges() const { return m_pickedEdges; }
    // Select at physical-pixel coords (shared by the mouse and the pick3d IPC
    // verb); returns a human-readable description of what got selected.
    // `additive` toggles the picked element in/out (Ctrl+click).
    QString pickAtPhysical(int px, int py, bool additive = false);
    // Pick at the physical centre of the view (for headless verification).
    QString pickCenter();

signals:
    // Emitted on pick: a short human-readable description of what's selected.
    void picked(const QString& info);
    // Right-click "Push/Pull" on a selected face: extrude it by `distance`.
    void pushPullFace(EntityId solid, double distance);
    // Right-click "Sketch on this face": set the work plane to the picked face.
    void sketchOnFace();
    // Right-click actions on the Ctrl-accumulated sub-shape selection (the
    // shapes are read back via pickedEdges()/pickedFaces()).
    void filletSelectedEdges(EntityId solid, double radius);
    void chamferSelectedEdges(EntityId solid, double distance);
    void shellOpenFaces(EntityId solid, double thickness);
    // Right-click "Split solid by this face's plane": split the owning solid
    // (pickedSolid) by the plane of the picked face.
    void splitByFace();
    // Command input was provided from the 3D view (same contract as
    // CanvasWidget::interaction): the main window refreshes prompt + views.
    void interaction();
    // Typing over the 3D view lands in the command bar (same contract as
    // CanvasWidget::typed; empty text = Enter with nothing active).
    void typed(const QString& text);
    // Run a command line through the shared processor (right-click Move…):
    // the host submits it exactly as if typed.
    void commandRequested(const QString& line);
    // Right-click on a bore wall: edit THAT hole (parametric, undoable).
    void moveHoleRequested(EntityId solid, int nodeIndex, double cx, double cy);
    void holeDiameterRequested(EntityId solid, int nodeIndex, double diameter);
    // One-line user feedback (e.g. why a context-menu action was refused);
    // routed to the command bar history by the host.
    void feedback(const QString& message);

protected:
    QPaintEngine* paintEngine() const override { return nullptr; }
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void initViewer();
    // Logical (Qt) -> physical (OCCT) pixel mapping for HiDPI displays.
    QPoint devicePos(const QPoint& logical) const;

    // Command-input pipeline (active-command Point prompts only).
    bool commandWantsPoint() const;
    // Labelled X/Y/Z axes following the cursor on the work plane while a
    // command asks for a point — no more moving blind.
    void updateAxes(const Vec2d& uv);
    void clearAxes();
    // Resolve the cursor into work-plane 2D coords: prefer the surface point of
    // the planar face under the cursor (which also becomes the work plane and
    // snaps to the face's feature points); fall back to a ray/work-plane
    // intersection in free space. Updates m_hover* members.
    bool cursorToPlane(const QPoint& physical, Vec2d& uv);
    void updateGhost(const Vec2d& uv);
    void clearGhost();
    // Right-click menu, synthesized on a SHORT right click (a right DRAG
    // orbits the camera, so Qt's automatic press-time menu is suppressed).
    void showContextMenu(const QPoint& globalPos);
    // Left-drag box selection (window select) with an OCCT rubber band.
    void updateRubberBand(const QPoint& fromLogical, const QPoint& toLogical);
    void finishBoxSelect(const QPoint& fromLogical, const QPoint& toLogical);

    Handle(V3d_Viewer) m_viewer;
    Handle(V3d_View) m_view;
    Handle(AIS_InteractiveContext) m_context;
    QPoint m_lastPos;
    QPoint m_pressPos;
    bool m_initFailed = false;
    bool m_fittedOnce = false;
    Qt::MouseButton m_orbitButton = Qt::RightButton; // drag orbits; short click = menu
    Qt::MouseButton m_panButton = Qt::MiddleButton;
    bool m_zoomInvert = false;
    // Until the user orbits/pans/zooms, window resizes re-frame the scene
    // (fixes the tiny-model-in-a-corner startup); afterwards the camera is his.
    bool m_userNavigated = false;

    Document* m_doc = nullptr;
    CommandProcessor* m_processor = nullptr;
    SelectionSet* m_selection = nullptr;

    // Map each displayed shape back to its document entity, and remember the
    // last picked face + its owning solid (for Push/Pull).
    std::vector<std::pair<Handle(AIS_InteractiveObject), EntityId>> m_shapes;
    TopoDS_Shape m_pickedFace;
    EntityId m_pickedSolid = kInvalidEntityId;
    std::vector<TopoDS_Shape> m_pickedFaces; // Ctrl-accumulated, one solid
    std::vector<TopoDS_Shape> m_pickedEdges;
    // A sub-shape pick just highlighted the face/edge itself: the next
    // syncHighlight must not replace it with a whole-solid highlight (that
    // was why "clicking the hole selected the solid").
    bool m_keepPickHighlight = false;

    // Transient ghost of the active command's pending result.
    Handle(AIS_InteractiveObject) m_ghost;
    // Clickable orientation cube pinned to the top-right corner.
    Handle(AIS_ViewCube) m_viewCube;
    // Cursor-following axes shown during point input.
    Handle(AIS_InteractiveObject) m_axes;
    // Left-drag window-selection rubber band.
    Handle(AIS_InteractiveObject) m_band;
    bool m_banding = false;
    // What the cursor last resolved to during a Point prompt.
    bool m_hoverValid = false;
    Vec2d m_hoverUv;
    EntityId m_hoverSolid = kInvalidEntityId;
};

} // namespace viki
