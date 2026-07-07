#include "OcctViewWidget.h"

#include <QMouseEvent>
#include <QWheelEvent>

#include <AIS_Shape.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Quantity_Color.hxx>
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
    int shown = 0;
    for (const EntityId id : doc.drawOrder()) {
        const auto* solid = dynamic_cast<const SolidEntity*>(doc.entity(id));
        if (!solid || solid->shape().IsNull())
            continue;
        Handle(AIS_Shape) ais = new AIS_Shape(solid->shape());
        ais->SetColor(Quantity_Color(0.72, 0.75, 0.78, Quantity_TOC_RGB));
        m_context->Display(ais, AIS_Shaded, 0, false);
        ++shown;
    }
    if (shown > 0)
        m_view->FitAll(0.1, false);
    m_view->Redraw();
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

void OcctViewWidget::mousePressEvent(QMouseEvent* event)
{
    m_lastPos = event->pos();
    if (!m_view.IsNull() && event->button() == Qt::LeftButton)
        m_view->StartRotation(event->pos().x(), event->pos().y());
}

void OcctViewWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_view.IsNull())
        return;
    if (event->buttons() & Qt::LeftButton) {
        m_view->Rotation(event->pos().x(), event->pos().y());
    } else if (event->buttons() & Qt::MiddleButton) {
        m_view->Pan(event->pos().x() - m_lastPos.x(),
                    m_lastPos.y() - event->pos().y());
    }
    m_lastPos = event->pos();
}

void OcctViewWidget::wheelEvent(QWheelEvent* event)
{
    if (m_view.IsNull())
        return;
    const double factor = event->angleDelta().y() > 0 ? 1.2 : 1.0 / 1.2;
    m_view->SetZoom(factor);
}

} // namespace viki
