#include "OcctViewWidget.h"

#include <algorithm>
#include <cstdint>

#include <QContextMenuEvent>
#include <QInputDialog>
#include <QMenu>
#include <QMouseEvent>
#include <QStringList>
#include <QWheelEvent>

#include <AIS_Shape.hxx>
#include <Aspect_Window.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_TypeOfHighlight.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Quantity_Color.hxx>
#include <StdSelect_BRepOwner.hxx>
#include <TopAbs.hxx>
#include <TopoDS_Shape.hxx>
#include <Xw_Window.hxx>

#include "solid/SolidEntity.h"

namespace viki {

OcctViewWidget::OcctViewWidget(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_NoSystemBackground);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
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
    } catch (...) {
        // No GL/X available (offscreen runs): degrade gracefully.
        m_initFailed = true;
        m_viewer.Nullify();
        m_view.Nullify();
        m_context.Nullify();
    }
}

void OcctViewWidget::refreshFrom(const Document& doc)
{
    initViewer();
    if (m_view.IsNull())
        return;
    m_context->RemoveAll(false);
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
        // Selectable/highlightable on hover: whole solid, faces and edges.
        m_context->Activate(ais, 0); // whole shape
        m_context->Activate(ais, AIS_Shape::SelectionMode(TopAbs_FACE));
        m_context->Activate(ais, AIS_Shape::SelectionMode(TopAbs_EDGE));
        ++shown;
    }
    if (shown > 0)
        m_view->FitAll(0.1, false);
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
    if (!m_view.IsNull())
        m_view->Redraw();
}

void OcctViewWidget::resizeEvent(QResizeEvent*)
{
    if (!m_view.IsNull())
        m_view->MustBeResized();
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

void OcctViewWidget::mousePressEvent(QMouseEvent* event)
{
    m_lastPos = event->pos();
    m_pressPos = event->pos();
    if (!m_view.IsNull() && event->button() == Qt::LeftButton) {
        const QPoint p = devicePos(event->pos());
        m_view->StartRotation(p.x(), p.y());
    }
}

void OcctViewWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_view.IsNull())
        return;
    const QPoint p = devicePos(event->pos());
    if (event->buttons() & Qt::LeftButton) {
        m_view->Rotation(p.x(), p.y());
    } else if (event->buttons() & Qt::MiddleButton) {
        const QPoint last = devicePos(m_lastPos);
        m_view->Pan(p.x() - last.x(), last.y() - p.y());
    } else if (!m_context.IsNull()) {
        // No button: dynamic hover highlight of the face/edge under the cursor.
        m_context->MoveTo(p.x(), p.y(), m_view, Standard_True);
    }
    m_lastPos = event->pos();
}

QString OcctViewWidget::pickAtPhysical(int px, int py)
{
    if (m_view.IsNull() || m_context.IsNull())
        return QStringLiteral("3D: no view");
    m_context->MoveTo(px, py, m_view, Standard_False);
    m_context->SelectDetected();

    m_pickedFace = TopoDS_Shape();
    m_pickedSolid = kInvalidEntityId;
    int solids = 0, faces = 0, edges = 0, verts = 0;
    for (m_context->InitSelected(); m_context->MoreSelected(); m_context->NextSelected()) {
        Handle(StdSelect_BRepOwner) owner =
            Handle(StdSelect_BRepOwner)::DownCast(m_context->SelectedOwner());
        if (owner.IsNull() || !owner->HasShape()) {
            ++solids;
            continue;
        }
        switch (owner->Shape().ShapeType()) {
        case TopAbs_FACE:
            ++faces;
            if (m_pickedFace.IsNull()) { // remember the first face for Push/Pull
                m_pickedFace = owner->Shape();
                const Handle(AIS_InteractiveObject) io =
                    Handle(AIS_InteractiveObject)::DownCast(owner->Selectable());
                for (const auto& pr : m_shapes)
                    if (pr.first == io)
                        m_pickedSolid = pr.second;
            }
            break;
        case TopAbs_EDGE: ++edges; break;
        case TopAbs_VERTEX: ++verts; break;
        default: ++solids; break;
        }
    }
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
    // A click (no meaningful drag) selects; a drag was an orbit.
    if ((event->pos() - m_pressPos).manhattanLength() >= 4)
        return;
    const QPoint p = devicePos(event->pos());
    emit picked(pickAtPhysical(p.x(), p.y()));
}

void OcctViewWidget::wheelEvent(QWheelEvent* event)
{
    if (m_view.IsNull())
        return;
    const double factor = event->angleDelta().y() > 0 ? 1.2 : 1.0 / 1.2;
    m_view->SetZoom(factor);
}

void OcctViewWidget::contextMenuEvent(QContextMenuEvent* event)
{
    // Right-click a selected face -> Push/Pull (extrude that face).
    if (m_pickedFace.IsNull() || m_pickedSolid == kInvalidEntityId) {
        QWidget::contextMenuEvent(event);
        return;
    }
    QMenu menu(this);
    QAction* pp = menu.addAction(QStringLiteral("Push/Pull face…"));
    if (menu.exec(event->globalPos()) != pp)
        return;
    bool ok = false;
    const double d = QInputDialog::getDouble(
        this, QStringLiteral("Push / Pull"),
        QStringLiteral("Distance (+ adds a boss, − cuts a pocket):"), 5.0,
        -1.0e6, 1.0e6, 3, &ok);
    if (ok && std::fabs(d) > 1e-9)
        emit pushPullFace(m_pickedSolid, d);
}

} // namespace viki
