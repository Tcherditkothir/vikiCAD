#include "CanvasWidget.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include "render/HitTest.h"

namespace viki {

namespace {
const QColor kBackground(24, 26, 28);
const QColor kCrosshair(140, 140, 140);
const QColor kSelection(64, 200, 255);
const QColor kPreview(255, 210, 90);
const QColor kRubberWindow(90, 140, 255);
const QColor kRubberCrossing(90, 220, 120);
} // namespace

CanvasWidget::CanvasWidget(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::BlankCursor);
}

void CanvasWidget::attach(Document* doc, CommandProcessor* processor, SelectionSet* selection)
{
    m_doc = doc;
    m_processor = processor;
    m_selection = selection;
    if (m_doc)
        m_doc->addChangeListener([this] { markDocumentDirty(); });
    m_staticDirty = true;
    update();
}

void CanvasWidget::markDocumentDirty()
{
    m_staticDirty = true;
    update();
}

void CanvasWidget::zoomExtents()
{
    if (!m_doc)
        return;
    m_camera.setViewport(size());
    m_camera.zoomExtents(m_doc->extents());
    markDocumentDirty();
}

void CanvasWidget::rebuildStaticLayer()
{
    m_staticLayer = QPixmap(size() * devicePixelRatioF());
    m_staticLayer.setDevicePixelRatio(devicePixelRatioF());
    m_staticLayer.fill(kBackground);
    if (!m_doc)
        return;

    QPainter painter(&m_staticLayer);
    painter.setRenderHint(QPainter::Antialiasing);

    RenderContext ctx;
    ctx.chordTolerance = m_camera.pixelsToWorld(0.25);

    for (const EntityId id : m_doc->drawOrder()) {
        const Entity* e = m_doc->entity(id);
        if (!e)
            continue;
        const Layer* layer = m_doc->layer(e->layerId());
        if (layer && !layer->visible)
            continue;
        ctx.resolvedColor = m_doc->resolveColor(*e);
        PrimitiveList list;
        e->buildPrimitives(ctx, list);
        drawPrimitives(painter, list);
    }
    m_staticDirty = false;
}

void CanvasWidget::drawPrimitives(QPainter& painter, const PrimitiveList& list,
                                  const QColor& overrideColor) const
{
    for (const StrokePrimitive& s : list.strokes) {
        if (s.points.size() < 2)
            continue;
        QPen pen(overrideColor.isValid() ? overrideColor : QColor::fromRgb(s.rgb));
        pen.setWidthF(overrideColor.isValid() ? 2.0 : 0.0); // 0 = cosmetic 1px
        pen.setCosmetic(true);
        painter.setPen(pen);

        QPainterPath path(m_camera.worldToScreen(s.points.front()));
        for (size_t i = 1; i < s.points.size(); ++i)
            path.lineTo(m_camera.worldToScreen(s.points[i]));
        if (s.closed)
            path.closeSubpath();
        painter.drawPath(path);
    }
}

void CanvasWidget::paintEvent(QPaintEvent*)
{
    m_camera.setViewport(size());
    if (m_staticDirty || m_staticLayer.size() != size() * devicePixelRatioF())
        rebuildStaticLayer();

    QPainter p(this);
    p.drawPixmap(0, 0, m_staticLayer);
    p.setRenderHint(QPainter::Antialiasing);

    // Selection highlight.
    if (m_doc && m_selection && !m_selection->isEmpty()) {
        RenderContext ctx;
        ctx.chordTolerance = m_camera.pixelsToWorld(0.25);
        for (const EntityId id : m_selection->ids()) {
            const Entity* e = m_doc->entity(id);
            if (!e)
                continue;
            PrimitiveList list;
            e->buildPrimitives(ctx, list);
            drawPrimitives(p, list, kSelection);
        }
    }

    // Active-command preview (rubber line/circle...).
    if (m_processor && m_processor->hasActiveCommand()) {
        PrimitiveList preview;
        m_processor->activeCommand()->previewAt(
            m_processor->ctx(), m_camera.screenToWorld(m_cursorPx), preview);
        if (!preview.strokes.empty()) {
            QPen pen(kPreview);
            pen.setCosmetic(true);
            pen.setStyle(Qt::DashLine);
            p.setPen(pen);
            for (const StrokePrimitive& s : preview.strokes) {
                if (s.points.size() < 2)
                    continue;
                QPainterPath path(m_camera.worldToScreen(s.points.front()));
                for (size_t i = 1; i < s.points.size(); ++i)
                    path.lineTo(m_camera.worldToScreen(s.points[i]));
                if (s.closed)
                    path.closeSubpath();
                p.drawPath(path);
            }
        }
    }

    // Rubber-band selection rectangle.
    if (m_rubberBand) {
        const bool window = m_cursorPx.x() >= m_rubberStart.x();
        QColor c = window ? kRubberWindow : kRubberCrossing;
        p.setPen(QPen(c, 0, window ? Qt::SolidLine : Qt::DashLine));
        c.setAlpha(40);
        p.setBrush(c);
        p.drawRect(QRectF(m_rubberStart, m_cursorPx).normalized());
        p.setBrush(Qt::NoBrush);
    }

    // Crosshair.
    p.setPen(QPen(kCrosshair, 0));
    p.drawLine(QPointF(0, m_cursorPx.y()), QPointF(width(), m_cursorPx.y()));
    p.drawLine(QPointF(m_cursorPx.x(), 0), QPointF(m_cursorPx.x(), height()));
}

void CanvasWidget::resizeEvent(QResizeEvent*)
{
    m_staticDirty = true;
}

void CanvasWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton) {
        m_panning = true;
        m_panLast = event->position();
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (event->button() == Qt::LeftButton) {
        const Vec2d world = m_camera.screenToWorld(event->position());
        // During a point-consuming command, clicks are input, not selection.
        if (m_processor && m_processor->hasActiveCommand()) {
            handleLeftClick(world, event->modifiers());
            return;
        }
        // Otherwise: pick or start a rubber band, decided on release/move.
        m_rubberBand = true;
        m_rubberStart = event->position();
    }
}

void CanvasWidget::mouseMoveEvent(QMouseEvent* event)
{
    m_cursorPx = event->position();
    if (m_panning) {
        m_camera.panPixels(event->position() - m_panLast);
        m_panLast = event->position();
        m_staticDirty = true;
    }
    update();
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton && m_panning) {
        m_panning = false;
        setCursor(Qt::BlankCursor);
        return;
    }
    if (event->button() == Qt::LeftButton && m_rubberBand) {
        m_rubberBand = false;
        finishRubberBand(event->position(), event->modifiers());
        update();
    }
}

void CanvasWidget::handleLeftClick(const Vec2d& world, Qt::KeyboardModifiers)
{
    switch (m_processor->currentRequest().kind) {
    case InputKind::Point:
    case InputKind::Distance:
        m_processor->provideInput(InputValue::makePoint(world));
        break;
    case InputKind::EntitySet: {
        const EntityId id = hittest::pick(*m_doc, world, pickTolerance());
        if (id != kInvalidEntityId)
            m_processor->provideInput(InputValue::makeEntityRef(id));
        break;
    }
    default:
        break;
    }
    emit interaction();
    update();
}

void CanvasWidget::finishRubberBand(const QPointF& releasePos, Qt::KeyboardModifiers mods)
{
    if (!m_doc || !m_selection)
        return;
    const bool click = (releasePos - m_rubberStart).manhattanLength() < 4.0;
    const bool keep = mods.testFlag(Qt::ShiftModifier);

    if (click) {
        const EntityId id =
            hittest::pick(*m_doc, m_camera.screenToWorld(releasePos), pickTolerance());
        if (!keep)
            m_selection->clear();
        if (id != kInvalidEntityId)
            m_selection->toggle(id);
    } else {
        const Vec2d a = m_camera.screenToWorld(m_rubberStart);
        const Vec2d b = m_camera.screenToWorld(releasePos);
        const BBox2d box(a, b);
        // Left-to-right = window (fully inside), right-to-left = crossing.
        const auto ids = releasePos.x() >= m_rubberStart.x()
                             ? hittest::window(*m_doc, box)
                             : hittest::crossing(*m_doc, box);
        if (!keep)
            m_selection->clear();
        for (const EntityId id : ids)
            m_selection->add(id);
    }
    emit interaction();
}

void CanvasWidget::wheelEvent(QWheelEvent* event)
{
    const double steps = event->angleDelta().y() / 120.0;
    m_camera.zoomAt(event->position(), std::pow(1.2, steps));
    m_staticDirty = true;
    update();
}

void CanvasWidget::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        if (m_processor && m_processor->hasActiveCommand())
            m_processor->cancelActive();
        else if (m_selection)
            m_selection->clear();
        emit interaction();
        update();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (m_processor && m_processor->hasActiveCommand()) {
            m_processor->provideInput(InputValue::makeFinish());
            emit interaction();
            update();
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

} // namespace viki
