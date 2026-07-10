#include "OcctViewWidget.h"

#include <algorithm>
#include <cstdint>

#include <QContextMenuEvent>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QStringList>
#include <QWheelEvent>

#include <AIS_AnimationCamera.hxx>
#include <AIS_SelectionScheme.hxx>
#include <AIS_Shape.hxx>
#include <AIS_Trihedron.hxx>
#include <AIS_ViewCube.hxx>
#include <Aspect_Window.hxx>
#include <Geom_Axis2Placement.hxx>
#include <Graphic3d_TransformPers.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_TypeOfHighlight.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Quantity_Color.hxx>
#include <StdSelect_BRepOwner.hxx>
#include <StdSelect_ViewerSelector3d.hxx>
#include <TopAbs.hxx>
#include <TopoDS_Shape.hxx>
#include <Xw_Window.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <Aspect_GridDrawMode.hxx>
#include <Aspect_GridType.hxx>

#include "cmd/Command.h"
#include "cmd/CommandProcessor.h"
#include "render/StandardViews.h"
#include "solid/SolidEntity.h"
#include "solid/SolidOps.h"

namespace viki {

OcctViewWidget::OcctViewWidget(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_NoSystemBackground);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    // Define an explicit cursor on this NATIVE window: with no cursor of its
    // own, X falls back to an inherited one, and a busy moment can leave the
    // pointer invisible over the GL surface. A crosshair also reads as
    // "placement" (Fusion-like).
    setCursor(Qt::CrossCursor);
}

void OcctViewWidget::initViewer()
{
    if (!m_view.IsNull() || m_initFailed)
        return;
    try {
        Handle(Aspect_DisplayConnection) display = new Aspect_DisplayConnection();
        Handle(OpenGl_GraphicDriver) driver = new OpenGl_GraphicDriver(display);
        m_viewer = new V3d_Viewer(driver);
        m_viewer->SetDefaultLights();
        m_viewer->SetLightOn();
        m_context = new AIS_InteractiveContext(m_viewer);
        m_view = m_viewer->CreateView();
        Handle(Xw_Window) window =
            new Xw_Window(display, static_cast<Aspect_Drawable>(winId()));
        m_view->SetWindow(window);
        if (!window->IsMapped())
            window->Map();
        m_view->SetBackgroundColor(Quantity_Color(0.09, 0.10, 0.11, Quantity_TOC_RGB));
        m_view->TriedronDisplay(Aspect_TOTP_LEFT_LOWER, Quantity_NOC_WHITE, 0.08);
        m_view->SetProj(V3d_XposYnegZpos); // isometric-ish start
        // Bold, distinct highlight colours: cyan on hover, orange when picked.
        // Faces/edges are SUB-shapes, so the Local* highlight types must be
        // set too — setting only Selected/Dynamic leaves them the faint grey
        // default (why a picked face didn't turn orange).
        const Quantity_Color cyan(0.20, 0.80, 1.0, Quantity_TOC_RGB);
        const Quantity_Color orange(1.0, 0.55, 0.0, Quantity_TOC_RGB);
        m_context->HighlightStyle(Prs3d_TypeOfHighlight_Dynamic)->SetColor(cyan);
        m_context->HighlightStyle(Prs3d_TypeOfHighlight_LocalDynamic)->SetColor(cyan);
        m_context->HighlightStyle(Prs3d_TypeOfHighlight_Selected)->SetColor(orange);
        m_context->HighlightStyle(Prs3d_TypeOfHighlight_LocalSelected)->SetColor(orange);
        m_context->HighlightStyle(Prs3d_TypeOfHighlight_LocalSelected)
            ->SetTransparency(0.0f);
        // The ViewCube (top-right corner): click faces/edges/corners to
        // orient the camera, Fusion-style. Clicks are routed in
        // mouseReleaseEvent (AIS_ViewCubeOwner -> HandleClick).
        m_viewCube = new AIS_ViewCube();
        m_viewCube->SetTransformPersistence(new Graphic3d_TransformPers(
            Graphic3d_TMF_TriedronPers, Aspect_TOTP_RIGHT_UPPER,
            Graphic3d_Vec2i(100, 100)));
        m_viewCube->SetSize(52.0);
        m_viewCube->SetBoxColor(Quantity_Color(0.62, 0.66, 0.72, Quantity_TOC_RGB));
        m_viewCube->SetTextColor(Quantity_NOC_BLACK);
        m_viewCube->SetViewAnimation(
            new AIS_AnimationCamera("viewcube", m_view));
        m_viewCube->SetFixedAnimationLoop(true);
        m_viewCube->SetDuration(0.2);
        m_context->Display(m_viewCube, false);
    } catch (...) {
        // No GL/X available (offscreen runs): degrade gracefully.
        m_initFailed = true;
        m_viewer.Nullify();
        m_view.Nullify();
        m_context.Nullify();
    }
}

void OcctViewWidget::attach(Document* doc, CommandProcessor* processor,
                            SelectionSet* selection)
{
    m_doc = doc;
    m_processor = processor;
    m_selection = selection;
    m_hoverValid = false;
    m_hoverSolid = kInvalidEntityId;
}

void OcctViewWidget::refreshFrom(const Document& doc)
{
    initViewer();
    if (m_view.IsNull())
        return;
    m_context->RemoveAll(false);
    m_ghost.Nullify(); // RemoveAll dropped it with everything else
    if (!m_viewCube.IsNull())
        m_context->Display(m_viewCube, false); // the cube survives rebuilds
    m_shapes.clear();
    m_pickedFace = TopoDS_Shape();
    m_pickedSolid = kInvalidEntityId;
    int shown = 0;
    for (const EntityId id : doc.drawOrder()) {
        const auto* solid = dynamic_cast<const SolidEntity*>(doc.entity(id));
        if (!solid || solid->shape().IsNull())
            continue;
        Handle(AIS_Shape) ais = new AIS_Shape(solid->shape());
        m_shapes.push_back({ais, id});
        const uint32_t rgb = doc.resolveColor(*solid);
        // White (default drawing colour) reads as a flat metal grey in 3D.
        const Quantity_Color col =
            rgb == 0xFFFFFF
                ? Quantity_Color(0.72, 0.75, 0.78, Quantity_TOC_RGB)
                : Quantity_Color(((rgb >> 16) & 0xFF) / 255.0,
                                 ((rgb >> 8) & 0xFF) / 255.0,
                                 (rgb & 0xFF) / 255.0, Quantity_TOC_RGB);
        ais->SetColor(col);
        if (solid->transparency > 0.0)
            ais->SetTransparency(Standard_ShortReal(
                std::min(solid->transparency, 0.95)));
        m_context->Display(ais, AIS_Shaded, 0, false);
        // Selectable/highlightable on hover: whole solid, faces, edges and
        // vertices (vertices feed the placement snap).
        m_context->Activate(ais, 0); // whole shape
        m_context->Activate(ais, AIS_Shape::SelectionMode(TopAbs_FACE));
        m_context->Activate(ais, AIS_Shape::SelectionMode(TopAbs_EDGE));
        m_context->Activate(ais, AIS_Shape::SelectionMode(TopAbs_VERTEX));
        // The document selection drives the orange highlight — a solid chosen
        // in the Assembly panel (or by a 3D click) stays visibly selected
        // across rebuilds.
        if (m_selection && m_selection->contains(id))
            m_context->AddOrRemoveSelected(ais, false);
        ++shown;
    }
    // Fit the camera on the first population only: refreshes now also happen
    // after every command (a hole appears in place), and re-fitting each time
    // would make the camera jump under the user's cursor. A STEP opened at
    // startup reaches here BEFORE the final window layout though, so the fit
    // is re-done on every resize until the user starts navigating (see
    // resizeEvent) — that is what actually frames the model once the window
    // reaches its real size.
    if (shown > 0 && !m_fittedOnce) {
        m_view->FitAll(0.1, false);
        m_fittedOnce = true;
    }
    m_view->Redraw();
}

void OcctViewWidget::syncHighlight()
{
    if (m_context.IsNull() || m_view.IsNull())
        return;
    // A face/edge was just picked in the view: OCCT already highlights that
    // precise element — do NOT swap it for a whole-solid highlight (that made
    // "clicking a hole select the whole solid" visually).
    if (m_keepPickHighlight) {
        m_keepPickHighlight = false;
        m_view->Redraw();
        return;
    }
    m_context->ClearSelected(false);
    if (m_selection)
        for (const auto& pr : m_shapes)
            if (m_selection->contains(pr.second))
                m_context->AddOrRemoveSelected(pr.first, false);
    m_view->Redraw();
}

bool OcctViewWidget::setStandardView(const QString& name)
{
    if (m_view.IsNull())
        return false;
    const auto o = views::standardViewDir(name);
    if (!o)
        return false;
    // OCCT's Proj vector points from the target TOWARD the eye — the reverse
    // of our look direction.
    m_view->SetProj(-o->dir.X(), -o->dir.Y(), -o->dir.Z());
    m_view->SetUp(o->up.X(), o->up.Y(), o->up.Z());
    m_view->FitAll(0.1, false);
    m_userNavigated = true; // deliberately placed — resizes keep it now
    m_view->Redraw();
    return true;
}

void OcctViewWidget::fitView()
{
    if (m_view.IsNull())
        return;
    m_view->FitAll(0.1, false);
    m_userNavigated = true;
    m_view->Redraw();
}

void OcctViewWidget::setGridVisible(bool on)
{
    if (m_viewer.IsNull())
        return;
    if (on) {
        // 10 mm reference grid in the XY plane.
        m_viewer->SetRectangularGridValues(0.0, 0.0, 10.0, 10.0, 0.0);
        m_viewer->ActivateGrid(Aspect_GT_Rectangular, Aspect_GDM_Lines);
    } else {
        m_viewer->DeactivateGrid();
    }
    if (!m_view.IsNull())
        m_view->Redraw();
}

bool OcctViewWidget::dumpToFile(const QString& path)
{
    if (m_view.IsNull())
        return false;
    m_view->Redraw();
    return m_view->Dump(path.toUtf8().constData()) == Standard_True;
}

void OcctViewWidget::paintEvent(QPaintEvent*)
{
    initViewer();
    if (m_view.IsNull())
        return;
    // Expose/paint arrives AFTER the X window truly has its new size, while
    // resizeEvent can run BEFORE X applied it — leaving a stale GL viewport
    // that squeezed the whole model into the bottom-left corner at startup.
    // Re-sync (cheap) and, until the user takes the camera, re-frame here too.
    m_view->MustBeResized();
    if (!m_userNavigated && !m_shapes.empty())
        m_view->FitAll(0.1, false);
    m_view->Redraw();
}

void OcctViewWidget::resizeEvent(QResizeEvent*)
{
    if (m_view.IsNull())
        return;
    m_view->MustBeResized();
    // A STEP opened at startup gets displayed (and fitted) before the window
    // reaches its final layout; MustBeResized alone keeps the stale camera and
    // the model ends up tiny in a corner. Until the user actually navigates
    // (orbit/pan/zoom), every resize re-frames the scene — after that, resizes
    // respect the user's camera.
    if (m_fittedOnce && !m_userNavigated && !m_shapes.empty()) {
        m_view->FitAll(0.1, false);
        m_view->Redraw();
    }
}

void OcctViewWidget::showEvent(QShowEvent*)
{
    initViewer();
}

// OCCT renders into the native window at PHYSICAL pixels; Qt reports mouse
// events at LOGICAL pixels. On a scaled (HiDPI) display the two differ, so
// picking must convert or MoveTo looks in the wrong place ("nothing under
// cursor" even over a solid). Derive the true ratio from OCCT's own window
// size vs the widget's logical size — robust to however Qt reports the DPR.
QPoint OcctViewWidget::devicePos(const QPoint& logical) const
{
    if (m_view.IsNull() || m_view->Window().IsNull() || width() <= 0 ||
        height() <= 0)
        return logical;
    Standard_Integer pw = 0, ph = 0;
    m_view->Window()->Size(pw, ph);
    if (pw <= 0 || ph <= 0)
        return logical;
    return QPoint(int(logical.x() * double(pw) / double(width())),
                  int(logical.y() * double(ph) / double(height())));
}

void OcctViewWidget::setMouseBindings(Qt::MouseButton orbit, Qt::MouseButton pan,
                                      bool zoomInvert)
{
    m_orbitButton = orbit;
    m_panButton = pan == orbit ? Qt::MiddleButton : pan; // never the same
    m_zoomInvert = zoomInvert;
}

void OcctViewWidget::mousePressEvent(QMouseEvent* event)
{
    m_lastPos = event->pos();
    m_pressPos = event->pos();
    if (!m_view.IsNull() && event->button() == m_orbitButton) {
        const QPoint p = devicePos(event->pos());
        m_view->StartRotation(p.x(), p.y());
    }
}

// Is the running command asking for a point-like input right now?
bool OcctViewWidget::commandWantsPoint() const
{
    if (!m_processor || !m_doc || !m_processor->hasActiveCommand())
        return false;
    const InputKind k = m_processor->currentRequest().kind;
    return k == InputKind::Point || k == InputKind::Distance;
}

// Resolve the cursor into work-plane 2D coordinates. Hovering a PLANAR face:
// that face becomes the active work plane (so a hole bores along its normal,
// like Fusion), the OCCT-detected surface point is projected into the plane,
// and the position snaps to the face's own feature points (hole centers, edge
// vertices). In free space: intersect the view ray with the current plane.
bool OcctViewWidget::cursorToPlane(const QPoint& physical, Vec2d& uv)
{
    m_hoverValid = false;
    m_hoverSolid = kInvalidEntityId;
    if (m_view.IsNull() || m_context.IsNull() || !m_doc)
        return false;

    // Walk EVERY pick candidate under the cursor, not just the topmost owner:
    // near an edge or a corner OCCT detects the EDGE/VERTEX first, and only
    // looking at the top one used to lose the face (no plane, no snap — why
    // snapping "worked on faces but not on vertices or hole rims").
    const auto sel = m_context->MainSelector();
    if (!sel.IsNull() && sel->NbPicked() > 0) {
        TopoDS_Shape face;
        gp_Pnt facePoint;
        bool haveVertex = false;
        gp_Pnt vertexPoint;
        Handle(SelectMgr_EntityOwner) faceOwner;
        for (Standard_Integer i = 1; i <= sel->NbPicked(); ++i) {
            Handle(StdSelect_BRepOwner) owner =
                Handle(StdSelect_BRepOwner)::DownCast(sel->Picked(i));
            if (owner.IsNull() || !owner->HasShape())
                continue;
            const TopAbs_ShapeEnum type = owner->Shape().ShapeType();
            if (type == TopAbs_VERTEX && !haveVertex) {
                haveVertex = true;
                vertexPoint = sel->PickedPoint(i); // exact corner snap
            } else if (type == TopAbs_FACE && face.IsNull()) {
                face = owner->Shape();
                facePoint = sel->PickedPoint(i);
                faceOwner = owner;
            }
        }
        std::optional<WorkPlane> plane;
        if (!face.IsNull())
            plane = solidops::planeFromFace(face);
        if (plane) {
            documentWorkplane(*m_doc) = *plane;
            const double tol = std::max(m_view->Convert(16), 1e-6);
            uv = solidops::projectToPlane2d(facePoint, *plane);
            if (haveVertex) {
                // A corner under the cursor wins: snap exactly to it.
                const Vec2d vuv = solidops::projectToPlane2d(vertexPoint, *plane);
                if (vuv.distanceTo(uv) <= tol)
                    uv = vuv;
            } else {
                // Otherwise snap to the face's real features (edge vertices,
                // hole/arc centers) within ~16 px.
                double best = tol;
                for (const SnapPoint& s :
                     solidops::faceSnapPoints2d(face, *plane)) {
                    const double d = s.p.distanceTo(uv);
                    if (d < best) {
                        best = d;
                        uv = s.p;
                    }
                }
            }
            // Remember the owning solid: a later EntitySet prompt (e.g.
            // HOLE's target) is answered by the same click.
            const Handle(AIS_InteractiveObject) io =
                Handle(AIS_InteractiveObject)::DownCast(faceOwner->Selectable());
            for (const auto& pr : m_shapes)
                if (pr.first == io)
                    m_hoverSolid = pr.second;
            m_hoverUv = uv;
            m_hoverValid = true;
            return true;
        }
    }

    // Free space: view ray ∩ current work plane.
    const WorkPlane plane = documentWorkplane(*m_doc);
    Standard_Real px = 0, py = 0, pz = 0, vx = 0, vy = 0, vz = 0;
    m_view->ConvertWithProj(physical.x(), physical.y(), px, py, pz, vx, vy, vz);
    const gp_Vec dir(vx, vy, vz);
    const gp_Vec n(plane.normal);
    const double denom = dir.Dot(n);
    if (std::fabs(denom) < 1e-9)
        return false; // ray parallel to the plane
    const gp_Vec toPlane(gp_Pnt(px, py, pz), plane.origin);
    const double t = toPlane.Dot(n) / denom;
    const gp_Pnt hit(px + vx * t, py + vy * t, pz + vz * t);
    uv = solidops::projectToPlane2d(hit, plane);
    m_hoverUv = uv;
    m_hoverValid = true;
    return true;
}

void OcctViewWidget::updateGhost(const Vec2d& uv)
{
    if (m_context.IsNull() || !m_processor || !m_processor->hasActiveCommand()) {
        const bool had = !m_ghost.IsNull();
        clearGhost();
        if (!had && !m_view.IsNull())
            m_view->Redraw(); // still flush the hover highlight
        return;
    }
    Preview3d preview;
    if (!m_processor->activeCommand()->preview3d(m_processor->ctx(), uv, preview) ||
        preview.shape.IsNull()) {
        const bool had = !m_ghost.IsNull();
        clearGhost();
        if (!had && !m_view.IsNull())
            m_view->Redraw();
        return;
    }
    if (!m_ghost.IsNull())
        m_context->Remove(m_ghost, false);
    Handle(AIS_Shape) ghost = new AIS_Shape(preview.shape);
    // Fusion colour language: red = material removed, blue = added.
    Quantity_Color col(0.70, 0.72, 0.75, Quantity_TOC_RGB);
    if (preview.effect == Preview3d::Effect::Remove)
        col = Quantity_Color(0.95, 0.25, 0.20, Quantity_TOC_RGB);
    else if (preview.effect == Preview3d::Effect::Add)
        col = Quantity_Color(0.25, 0.55, 0.95, Quantity_TOC_RGB);
    ghost->SetColor(col);
    ghost->SetTransparency(0.55f);
    m_context->Display(ghost, AIS_Shaded, -1, false); // -1: not selectable
    m_ghost = ghost;
    m_view->Redraw();
}

// Labelled X/Y/Z axes at the cursor position on the work plane: during MOVE3D
// or hole placement you SEE which way the axes run instead of guessing.
void OcctViewWidget::updateAxes(const Vec2d& uv)
{
    if (m_context.IsNull() || !m_doc)
        return;
    const WorkPlane wp = documentWorkplane(*m_doc);
    gp_Ax2 placement;
    try {
        placement = gp_Ax2(solidops::planePoint3d(uv, wp), wp.normal, wp.xDir);
    } catch (const Standard_Failure&) {
        return; // degenerate plane frame — skip the marker this move
    }
    Handle(AIS_Trihedron) tri = Handle(AIS_Trihedron)::DownCast(m_axes);
    if (tri.IsNull()) {
        tri = new AIS_Trihedron(new Geom_Axis2Placement(placement));
        tri->SetSize(18.0);
        m_axes = tri;
        m_context->Display(m_axes, false);
        m_context->Deactivate(m_axes); // a marker, never a pick target
    } else {
        tri->SetComponent(new Geom_Axis2Placement(placement));
        m_context->Redisplay(m_axes, false);
    }
}

void OcctViewWidget::clearAxes()
{
    if (m_axes.IsNull())
        return;
    if (!m_context.IsNull())
        m_context->Remove(m_axes, false);
    m_axes.Nullify();
}

void OcctViewWidget::clearGhost()
{
    clearAxes(); // the axes marker shares the ghost's lifecycle
    if (m_ghost.IsNull())
        return;
    if (!m_context.IsNull()) {
        m_context->Remove(m_ghost, false);
        if (!m_view.IsNull())
            m_view->Redraw();
    }
    m_ghost.Nullify();
}

void OcctViewWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_view.IsNull())
        return;
    const QPoint p = devicePos(event->pos());
    if (event->buttons() & m_orbitButton) {
        // A real drag (not a click) = the user took over the camera.
        if ((event->pos() - m_pressPos).manhattanLength() >= 4)
            m_userNavigated = true;
        m_view->Rotation(p.x(), p.y());
    } else if (event->buttons() & m_panButton) {
        m_userNavigated = true;
        const QPoint last = devicePos(m_lastPos);
        m_view->Pan(p.x() - last.x(), last.y() - p.y());
    } else if (!m_context.IsNull()) {
        // No button: dynamic hover highlight of the face/edge under the cursor.
        // In command-input mode we redraw ONCE ourselves (ghost included) —
        // two full GL redraws per mouse move made big parts feel frozen.
        const bool wantPoint = commandWantsPoint();
        m_context->MoveTo(p.x(), p.y(), m_view,
                          wantPoint ? Standard_False : Standard_True);
        if (wantPoint) {
            Vec2d uv;
            if (cursorToPlane(p, uv)) {
                m_processor->ctx().setPointerHint(uv);
                updateAxes(uv);  // show the axis directions at the cursor
                updateGhost(uv); // redraws
            } else {
                m_view->Redraw(); // flush the MoveTo highlight
            }
        } else if (!m_ghost.IsNull() || !m_axes.IsNull()) {
            clearGhost(); // the command ended some other way (axes too)
            m_view->Redraw();
        }
    }
    m_lastPos = event->pos();
}

QString OcctViewWidget::pickAtPhysical(int px, int py, bool additive)
{
    if (m_view.IsNull() || m_context.IsNull())
        return QStringLiteral("3D: no view");
    m_context->MoveTo(px, py, m_view, Standard_False);
    // Plain click replaces the selection; Ctrl+click toggles the element so
    // several faces/edges can be accumulated for one operation.
    m_context->SelectDetected(additive ? AIS_SelectionScheme_XOR
                                       : AIS_SelectionScheme_Replace);

    m_pickedFace = TopoDS_Shape();
    m_pickedSolid = kInvalidEntityId;
    m_pickedFaces.clear();
    m_pickedEdges.clear();
    int solids = 0, faces = 0, edges = 0, verts = 0;
    // Resolve the OWNING SOLID for whatever got picked (face, edge, vertex or
    // whole shape) — plain clicks must always select the entity, not only
    // when a face happens to win the pick.
    const auto rememberSolid = [this](const Handle(SelectMgr_EntityOwner)& ow) {
        if (m_pickedSolid != kInvalidEntityId || ow.IsNull())
            return;
        const Handle(AIS_InteractiveObject) io =
            Handle(AIS_InteractiveObject)::DownCast(ow->Selectable());
        for (const auto& pr : m_shapes)
            if (pr.first == io)
                m_pickedSolid = pr.second;
    };
    for (m_context->InitSelected(); m_context->MoreSelected(); m_context->NextSelected()) {
        rememberSolid(m_context->SelectedOwner());
        Handle(StdSelect_BRepOwner) owner =
            Handle(StdSelect_BRepOwner)::DownCast(m_context->SelectedOwner());
        if (owner.IsNull() || !owner->HasShape()) {
            ++solids;
            continue;
        }
        switch (owner->Shape().ShapeType()) {
        case TopAbs_FACE:
            ++faces;
            if (m_pickedFace.IsNull()) // remember the first face for Push/Pull
                m_pickedFace = owner->Shape();
            m_pickedFaces.push_back(owner->Shape());
            break;
        case TopAbs_EDGE:
            ++edges;
            m_pickedEdges.push_back(owner->Shape());
            break;
        case TopAbs_VERTEX: ++verts; break;
        default: ++solids; break;
        }
    }
    // A sub-shape got highlighted by OCCT itself (the face/edge turns orange,
    // not the whole solid) — tell the next syncHighlight to leave it alone.
    m_keepPickHighlight = faces + edges + verts > 0;
    // Show the selection highlight (the SelectionStyle colour) right away.
    m_context->UpdateCurrentViewer();
    QStringList parts;
    if (solids) parts << QStringLiteral("%1 solid").arg(solids);
    if (faces) parts << QStringLiteral("%1 face").arg(faces);
    if (edges) parts << QStringLiteral("%1 edge").arg(edges);
    if (verts) parts << QStringLiteral("%1 vertex").arg(verts);
    return parts.isEmpty() ? QStringLiteral("3D: nothing under cursor")
                           : QStringLiteral("3D selected: %1")
                                 .arg(parts.join(QStringLiteral(", ")));
}

QString OcctViewWidget::pickCenter()
{
    if (m_view.IsNull() || m_view->Window().IsNull())
        return QStringLiteral("3D: no view");
    Standard_Integer w = 0, h = 0;
    m_view->Window()->Size(w, h);
    return pickAtPhysical(w / 2, h / 2);
}

void OcctViewWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_view.IsNull() || m_context.IsNull() || event->button() != Qt::LeftButton)
        return;
    // A click (no meaningful drag) acts; a drag was an orbit.
    if ((event->pos() - m_pressPos).manhattanLength() >= 4)
        return;
    const QPoint p = devicePos(event->pos());

    // A click on the ViewCube orients the camera — nothing else.
    m_context->MoveTo(p.x(), p.y(), m_view, Standard_False);
    Handle(AIS_ViewCubeOwner) cubeOwner =
        Handle(AIS_ViewCubeOwner)::DownCast(m_context->DetectedOwner());
    if (!cubeOwner.IsNull() && !m_viewCube.IsNull()) {
        m_viewCube->HandleClick(cubeOwner);
        m_userNavigated = true; // deliberate camera placement
        m_view->Redraw();
        return;
    }

    // A running command asking for a point: the click IS the point (work-plane
    // coords resolved by the last hover). If the command then asks for an
    // entity and the click was on a solid's face, the same click answers that
    // too — HOLE 6 T + one click on a face = a hole in that solid.
    if (commandWantsPoint()) {
        Vec2d uv;
        if (m_hoverValid)
            uv = m_hoverUv;
        else if (!cursorToPlane(p, uv))
            return;
        const EntityId hitSolid = m_hoverSolid;
        m_processor->provideInput(InputValue::makePoint(uv));
        if (m_processor->hasActiveCommand() &&
            m_processor->currentRequest().kind == InputKind::EntitySet &&
            hitSolid != kInvalidEntityId) {
            m_processor->provideInput(InputValue::makeEntityRef(hitSolid));
        }
        if (!m_processor->hasActiveCommand())
            clearGhost();
        emit interaction();
        return;
    }

    // A running command asking for entities: clicking a solid supplies it
    // (same contract as the 2D canvas hit-test click).
    if (m_processor && m_doc && m_processor->hasActiveCommand() &&
        m_processor->currentRequest().kind == InputKind::EntitySet) {
        pickAtPhysical(p.x(), p.y());
        if (m_pickedSolid != kInvalidEntityId) {
            m_processor->provideInput(InputValue::makeEntityRef(m_pickedSolid));
            if (!m_processor->hasActiveCommand())
                clearGhost();
            emit interaction();
            return;
        }
    }

    // No command running:
    // - plain click SELECTS the picked solid (document selection: Properties,
    //   Assembly row, highlight) while OCCT highlights the precise face/edge;
    // - Ctrl+click ACCUMULATES faces/edges (XOR) for a multi-element
    //   operation (fillet, chamfer, shell-open…) without touching the
    //   document selection. Empty plain click clears everything.
    const bool ctrl = event->modifiers().testFlag(Qt::ControlModifier);
    const QString info = pickAtPhysical(p.x(), p.y(), ctrl);
    if (!ctrl && m_selection) {
        if (m_pickedSolid != kInvalidEntityId) {
            if (!(m_selection->size() == 1 &&
                  m_selection->contains(m_pickedSolid))) {
                m_selection->clear();
                m_selection->add(m_pickedSolid);
            }
        } else {
            m_selection->clear();
        }
        emit interaction();
    }
    emit picked(info);
}

void OcctViewWidget::wheelEvent(QWheelEvent* event)
{
    if (m_view.IsNull())
        return;
    m_userNavigated = true;
    const bool up = m_zoomInvert ? event->angleDelta().y() < 0
                                 : event->angleDelta().y() > 0;
    m_view->SetZoom(up ? 1.2 : 1.0 / 1.2);
}

// Same typing contract as the 2D canvas: no need to click the command bar —
// type anywhere over the 3D view and the characters land in the bar.
void OcctViewWidget::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        if (m_processor && m_processor->hasActiveCommand())
            m_processor->cancelActive();
        else if (m_selection)
            m_selection->clear();
        clearGhost();
        emit interaction();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
        event->key() == Qt::Key_Space) {
        if (m_processor && m_processor->hasActiveCommand()) {
            m_processor->provideInput(InputValue::makeFinish());
            if (!m_processor->hasActiveCommand())
                clearGhost();
            emit interaction();
            return;
        }
        emit typed(QString()); // empty = repeat-last handled upstream
        return;
    }
    const QString text = event->text();
    if (!text.isEmpty() && text[0].isPrint()) {
        emit typed(text);
        return;
    }
    QWidget::keyPressEvent(event);
}

void OcctViewWidget::contextMenuEvent(QContextMenuEvent* event)
{
    // Scene rebuilds (after push/pull, undo…) clear the pick state, which
    // used to make right-click dead until the user left-clicked again. Fusion
    // semantics instead: right-click acts on WHAT IS UNDER THE CURSOR — pick
    // it now if nothing is currently picked (a Ctrl-accumulated multi-pick is
    // kept as-is).
    if (m_pickedSolid == kInvalidEntityId && !m_view.IsNull() &&
        !m_context.IsNull()) {
        const QPoint p = devicePos(event->pos());
        pickAtPhysical(p.x(), p.y());
    }
    if (m_pickedSolid == kInvalidEntityId) {
        QWidget::contextMenuEvent(event);
        return;
    }
    QMenu menu(this);
    // The picked face is a bore wall? Offer to edit THAT hole first — moving
    // the hole, not the solid, is what a click on a hole means.
    int holeNode = -1;
    const FeatureTree* tree = nullptr;
    if (m_doc && !m_pickedFace.IsNull()) {
        const auto* solidEnt =
            dynamic_cast<const SolidEntity*>(m_doc->entity(m_pickedSolid));
        if (solidEnt && solidEnt->features) {
            holeNode = featureForFace(*solidEnt->features, m_pickedFace);
            tree = solidEnt->features.get();
        }
    }
    QAction* mvHole = nullptr;
    QAction* diaHole = nullptr;
    if (holeNode >= 0) {
        mvHole = menu.addAction(
            QStringLiteral("Move hole %1… (center x, y)").arg(holeNode));
        diaHole = menu.addAction(QStringLiteral("Hole %1 diameter…").arg(holeNode));
        menu.addSeparator();
    }
    // Always available on a picked solid: move it (typed offset or two
    // picked points — the two-point flow then clicks the solid to move).
    QAction* mvNum = menu.addAction(QStringLiteral("Move solid… (dx, dy, dz)"));
    QAction* mvPts = menu.addAction(QStringLiteral("Move by two points"));
    menu.addSeparator();
    QAction* pp = nullptr;
    QAction* sk = nullptr;
    QAction* sp = nullptr;
    QAction* sh = nullptr;
    if (!m_pickedFace.IsNull()) {
        pp = menu.addAction(QStringLiteral("Push/Pull face…"));
        sk = menu.addAction(QStringLiteral("Sketch on this face"));
        sp = menu.addAction(QStringLiteral("Split solid by this face"));
        sh = menu.addAction(QStringLiteral("Shell — keep %1 face(s) open…")
                                .arg(m_pickedFaces.size()));
    }
    QAction* fe = nullptr;
    QAction* ce = nullptr;
    if (!m_pickedEdges.empty()) {
        fe = menu.addAction(QStringLiteral("Fillet %1 selected edge(s)…")
                                .arg(m_pickedEdges.size()));
        ce = menu.addAction(QStringLiteral("Chamfer %1 selected edge(s)…")
                                .arg(m_pickedEdges.size()));
    }
    QAction* chosen = menu.exec(event->globalPos());
    bool ok = false;
    if (holeNode >= 0 && chosen == mvHole && tree) {
        const Vec2d cur = tree->nodeAt(holeNode).holeCenter;
        const QString text = QInputDialog::getText(
            this, QStringLiteral("Move hole"),
            QStringLiteral("New center x, y (bore-plane mm):"),
            QLineEdit::Normal,
            QStringLiteral("%1, %2").arg(cur.x).arg(cur.y), &ok);
        if (!ok)
            return; // user cancelled
        const QStringList parts = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
        bool okx = false, oky = false;
        const double cx = parts.value(0).trimmed().toDouble(&okx);
        const double cy = parts.value(1).trimmed().toDouble(&oky);
        if (parts.size() != 2 || !okx || !oky) {
            emit feedback(QStringLiteral(
                              "! move hole: expected two numbers \"x, y\" — got \"%1\"")
                              .arg(text));
            return;
        }
        emit moveHoleRequested(m_pickedSolid, holeNode, cx, cy);
        return;
    }
    if (holeNode >= 0 && chosen == diaHole && tree) {
        const double d = QInputDialog::getDouble(
            this, QStringLiteral("Hole diameter"),
            QStringLiteral("Diameter (mm):"), tree->nodeAt(holeNode).diameter,
            0.001, 1.0e6, 3, &ok);
        if (ok)
            emit holeDiameterRequested(m_pickedSolid, holeNode, d);
        return;
    }
    if (chosen == mvNum) {
        const QString text = QInputDialog::getText(
            this, QStringLiteral("Move solid"),
            QStringLiteral("Displacement dx, dy, dz (mm):"), QLineEdit::Normal,
            QStringLiteral("0, 0, 0"), &ok);
        if (!ok)
            return; // user cancelled
        const QStringList parts = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
        bool okx = false, oky = false, okz = false;
        const double dx = parts.value(0).trimmed().toDouble(&okx);
        const double dy = parts.value(1).trimmed().toDouble(&oky);
        const double dz = parts.value(2).trimmed().toDouble(&okz);
        if (parts.size() != 3 || !okx || !oky || !okz) {
            emit feedback(
                QStringLiteral(
                    "! move solid: expected three numbers \"dx, dy, dz\" — got \"%1\"")
                    .arg(text));
            return;
        }
        emit commandRequested(QStringLiteral("MOVE3D %1 %2 %3 %4")
                                  .arg(dx)
                                  .arg(dy)
                                  .arg(dz)
                                  .arg(qlonglong(m_pickedSolid)));
        return;
    }
    if (chosen == mvPts) {
        // Starts MOVE3D point mode: click the base point, the destination,
        // then the solid to move (all in the 3D view).
        emit commandRequested(QStringLiteral("MOVE3D P"));
        return;
    }
    if (chosen && chosen == pp) {
        const double d = QInputDialog::getDouble(
            this, QStringLiteral("Push / Pull"),
            QStringLiteral("Distance (+ adds a boss, − cuts a pocket):"), 5.0,
            -1.0e6, 1.0e6, 3, &ok);
        if (ok && std::fabs(d) > 1e-9)
            emit pushPullFace(m_pickedSolid, d);
        else if (ok)
            emit feedback(
                QStringLiteral("push/pull: distance 0 — nothing to do"));
    } else if (chosen && chosen == sk) {
        emit sketchOnFace();
    } else if (chosen && chosen == sp) {
        emit splitByFace();
    } else if (chosen && chosen == sh) {
        const double t = QInputDialog::getDouble(
            this, QStringLiteral("Shell"), QStringLiteral("Wall thickness (mm):"),
            2.0, 0.001, 1.0e6, 3, &ok);
        if (ok)
            emit shellOpenFaces(m_pickedSolid, t);
    } else if (chosen && chosen == fe) {
        const double r = QInputDialog::getDouble(
            this, QStringLiteral("Fillet"), QStringLiteral("Radius (mm):"), 1.0,
            0.001, 1.0e6, 3, &ok);
        if (ok)
            emit filletSelectedEdges(m_pickedSolid, r);
    } else if (chosen && chosen == ce) {
        const double d = QInputDialog::getDouble(
            this, QStringLiteral("Chamfer"), QStringLiteral("Distance (mm):"),
            1.0, 0.001, 1.0e6, 3, &ok);
        if (ok)
            emit chamferSelectedEdges(m_pickedSolid, d);
    }
}

} // namespace viki
