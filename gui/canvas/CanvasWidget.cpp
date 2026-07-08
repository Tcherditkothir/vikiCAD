#include "CanvasWidget.h"

#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include "render/HitTest.h"

namespace viki {

namespace {
const QColor kBackground(24, 26, 28);
const QColor kGridMinor(40, 43, 46);
const QColor kGridMajor(56, 60, 64);
const QColor kAxis(70, 76, 82);
const QColor kCrosshair(140, 140, 140);
const QColor kSelection(64, 200, 255);
const QColor kPreview(255, 210, 90);
const QColor kSnapGlyph(80, 255, 120);
const QColor kGrip(60, 130, 246);
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

void CanvasWidget::drawGrid(QPainter& painter) const
{
    const double pxPerMinor = m_gridSpacing * m_camera.scale();
    if (pxPerMinor < 5.0)
        return; // too dense — draw nothing rather than noise

    const Vec2d topLeft = m_camera.screenToWorld({0, 0});
    const Vec2d bottomRight = m_camera.screenToWorld({double(width()), double(height())});
    const double x0 = std::floor(topLeft.x / m_gridSpacing) * m_gridSpacing;
    const double y0 = std::floor(bottomRight.y / m_gridSpacing) * m_gridSpacing;

    for (double x = x0; x <= bottomRight.x; x += m_gridSpacing) {
        const bool major = nearZero(std::fmod(std::round(x / m_gridSpacing), 10.0));
        painter.setPen(QPen(nearZero(x) ? kAxis : (major ? kGridMajor : kGridMinor), 0));
        const double sx = m_camera.worldToScreen({x, 0}).x();
        painter.drawLine(QPointF(sx, 0), QPointF(sx, height()));
    }
    for (double y = y0; y <= topLeft.y; y += m_gridSpacing) {
        const bool major = nearZero(std::fmod(std::round(y / m_gridSpacing), 10.0));
        painter.setPen(QPen(nearZero(y) ? kAxis : (major ? kGridMajor : kGridMinor), 0));
        const double sy = m_camera.worldToScreen({0, y}).y();
        painter.drawLine(QPointF(0, sy), QPointF(width(), sy));
    }
}

void CanvasWidget::rebuildStaticLayer()
{
    m_staticLayer = QPixmap(size() * devicePixelRatioF());
    m_staticLayer.setDevicePixelRatio(devicePixelRatioF());
    m_staticLayer.fill(kBackground);
    if (!m_doc)
        return;

    QPainter painter(&m_staticLayer);

    if (m_gridSnap)
        drawGrid(painter);

    painter.setRenderHint(QPainter::Antialiasing);

    RenderContext ctx;
    ctx.chordTolerance = m_camera.pixelsToWorld(0.25);
    ctx.pixelSize = m_camera.pixelsToWorld(1.0);
    ctx.viewBox = BBox2d(m_camera.screenToWorld({0, double(height())}),
                         m_camera.screenToWorld({double(width()), 0}));
    ctx.doc = m_doc;

    for (const EntityId id : m_doc->drawOrder()) {
        const Entity* e = m_doc->entity(id);
        if (!e)
            continue;
        const Layer* layer = m_doc->layer(e->layerId());
        if (layer && !layer->visible)
            continue;
        // View culling.
        if (!m_doc->entityBounds(*e).intersects(ctx.viewBox))
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
        const QColor color =
            overrideColor.isValid() ? overrideColor : QColor::fromRgb(s.rgb);
        QPen pen(color);
        pen.setWidthF(overrideColor.isValid() ? 2.0 : 0.0); // 0 = cosmetic 1px
        pen.setCosmetic(true);
        painter.setPen(pen);

        QPainterPath path(m_camera.worldToScreen(s.points.front()));
        for (size_t i = 1; i < s.points.size(); ++i)
            path.lineTo(m_camera.worldToScreen(s.points[i]));
        if (s.closed)
            path.closeSubpath();
        if (s.filled)
            painter.fillPath(path, color);
        else
            painter.drawPath(path);
    }

    for (const TextPrimitive& t : list.texts) {
        const QColor color = overrideColor.isValid() ? overrideColor : QColor::fromRgb(t.rgb);
        const double px = t.height * m_camera.scale();
        if (px < 2.0) { // LOD: too small to read, draw a bar
            painter.setPen(QPen(color, 0));
            const QPointF a = m_camera.worldToScreen(t.pos);
            painter.drawLine(a, a + QPointF(t.text.size() * px * 0.6, 0));
            continue;
        }
        QFont font(QStringLiteral("DejaVu Sans"));
        font.setPixelSize(std::max(1, int(std::lround(px))));
        painter.save();
        painter.setPen(color);
        painter.setFont(font);
        const QPointF anchor = m_camera.worldToScreen(t.pos);
        painter.translate(anchor);
        painter.rotate(-t.rotation * 180.0 / M_PI);
        double dx = 0;
        const double w = QFontMetricsF(font).horizontalAdvance(t.text);
        if (t.hAlign == TextHAlign::Center)
            dx = -w / 2;
        else if (t.hAlign == TextHAlign::Right)
            dx = -w;
        painter.drawText(QPointF(dx, 0), t.text);
        painter.restore();
    }
}

void CanvasWidget::drawSnapGlyph(QPainter& p) const
{
    if (!m_activeSnap)
        return;
    const QPointF c = m_camera.worldToScreen(m_activeSnap->point);
    const double r = 6.0;
    p.setPen(QPen(kSnapGlyph, 2));
    p.setBrush(Qt::NoBrush);
    switch (m_activeSnap->kind) {
    case SnapKind::Endpoint: // square
        p.drawRect(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r));
        break;
    case SnapKind::Midpoint: // triangle
        p.drawPolygon(QPolygonF({{c.x(), c.y() - r},
                                 {c.x() - r, c.y() + r},
                                 {c.x() + r, c.y() + r}}));
        break;
    case SnapKind::Center: // circle
        p.drawEllipse(c, r, r);
        break;
    case SnapKind::Quadrant: // diamond
        p.drawPolygon(QPolygonF(
            {{c.x(), c.y() - r}, {c.x() + r, c.y()}, {c.x(), c.y() + r}, {c.x() - r, c.y()}}));
        break;
    case SnapKind::Intersection: // X
        p.drawLine(QPointF(c.x() - r, c.y() - r), QPointF(c.x() + r, c.y() + r));
        p.drawLine(QPointF(c.x() - r, c.y() + r), QPointF(c.x() + r, c.y() - r));
        break;
    case SnapKind::Perpendicular: // right-angle mark
        p.drawLine(QPointF(c.x() - r, c.y() - r), QPointF(c.x() - r, c.y() + r));
        p.drawLine(QPointF(c.x() - r, c.y() + r), QPointF(c.x() + r, c.y() + r));
        break;
    default:
        p.drawEllipse(c, 2, 2);
        break;
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
        ctx.pixelSize = m_camera.pixelsToWorld(1.0);
        ctx.viewBox = BBox2d(m_camera.screenToWorld({0, double(height())}),
                             m_camera.screenToWorld({double(width()), 0}));
        ctx.doc = m_doc;
        for (const EntityId id : m_selection->ids()) {
            const Entity* e = m_doc->entity(id);
            if (!e)
                continue;
            PrimitiveList list;
            e->buildPrimitives(ctx, list);
            drawPrimitives(p, list, kSelection);
        }
    }

    if (m_processor && !m_processor->hasActiveCommand())
        drawGrips(p);

    // Active-command preview, anchored at the effective (snapped) cursor.
    if (m_processor && m_processor->hasActiveCommand()) {
        PrimitiveList preview;
        m_processor->activeCommand()->previewAt(m_processor->ctx(), m_effectiveCursor,
                                                preview);
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

    // Grip drag ghost.
    if (m_grip.active && m_doc) {
        if (const Entity* e = m_doc->entity(m_grip.id)) {
            auto ghost = e->clone();
            ghost->moveGrip(m_grip.index, m_effectiveCursor);
            RenderContext gc;
            gc.chordTolerance = m_camera.pixelsToWorld(0.5);
            gc.pixelSize = m_camera.pixelsToWorld(1.0);
            gc.doc = m_doc;
            PrimitiveList gl;
            ghost->buildPrimitives(gc, gl);
            drawPrimitives(p, gl, kPreview);
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

    drawSnapGlyph(p);

    // Crosshair at the effective position (shows the snap/ortho pull).
    const QPointF cross = commandWantsPoint()
                              ? m_camera.worldToScreen(m_effectiveCursor)
                              : m_cursorPx;
    p.setPen(QPen(kCrosshair, 0));
    p.drawLine(QPointF(0, cross.y()), QPointF(width(), cross.y()));
    p.drawLine(QPointF(cross.x(), 0), QPointF(cross.x(), height()));
}

void CanvasWidget::resizeEvent(QResizeEvent*)
{
    m_staticDirty = true;
}

bool CanvasWidget::commandWantsPoint() const
{
    if (!m_processor || !m_processor->hasActiveCommand())
        return false;
    const InputKind k = m_processor->currentRequest().kind;
    return k == InputKind::Point || k == InputKind::Distance || k == InputKind::Number;
}

Vec2d CanvasWidget::effectivePoint(const Vec2d& cursorWorld)
{
    m_activeSnap.reset();
    if (!m_doc)
        return cursorWorld;

    // 1. Object snap (also active while dragging a grip).
    if (m_snapSettings.enabled && (commandWantsPoint() || m_grip.active)) {
        const std::optional<Vec2d> perpBase =
            m_processor->ctx().lastPoint().lengthSq() > 0
                ? std::optional<Vec2d>(m_processor->ctx().lastPoint())
                : std::nullopt;
        m_activeSnap = snapQuery(*m_doc, cursorWorld, m_camera.pixelsToWorld(10.0),
                                 m_snapSettings, perpBase);
        if (m_activeSnap)
            return m_activeSnap->point;
    }

    Vec2d p = cursorWorld;

    // 2. Ortho / polar relative to the rubber-band base.
    if (commandWantsPoint()) {
        const Vec2d base = m_processor->ctx().lastPoint();
        const Vec2d d = p - base;
        if (m_ortho && d.length() > kGeomTol) {
            p = std::fabs(d.x) >= std::fabs(d.y) ? Vec2d{p.x, base.y} : Vec2d{base.x, p.y};
        } else if (m_polar && d.length() > kGeomTol) {
            const double step = 15.0 * M_PI / 180.0;
            const double snapped = std::round(d.angle() / step) * step;
            p = base + Vec2d::polar(d.length(), snapped);
        }
    }

    // 3. Grid snap.
    if (m_gridSnap && !m_activeSnap) {
        p.x = std::round(p.x / m_gridSpacing) * m_gridSpacing;
        p.y = std::round(p.y / m_gridSpacing) * m_gridSpacing;
    }
    return p;
}

std::optional<std::pair<EntityId, int>> CanvasWidget::gripAt(const QPointF& px) const
{
    if (!m_doc || !m_selection)
        return std::nullopt;
    for (const EntityId id : m_selection->ids()) {
        const Entity* e = m_doc->entity(id);
        if (!e)
            continue;
        const auto grips = e->gripPoints();
        for (size_t i = 0; i < grips.size(); ++i) {
            const QPointF g = m_camera.worldToScreen(grips[i]);
            if (QLineF(g, px).length() <= 6.0)
                return std::make_pair(id, int(i));
        }
    }
    return std::nullopt;
}

void CanvasWidget::drawGrips(QPainter& p) const
{
    if (!m_doc || !m_selection || m_selection->isEmpty())
        return;
    p.setPen(QPen(Qt::white, 0));
    for (const EntityId id : m_selection->ids()) {
        const Entity* e = m_doc->entity(id);
        if (!e)
            continue;
        for (const Vec2d& g : e->gripPoints()) {
            const QPointF s = m_camera.worldToScreen(g);
            p.fillRect(QRectF(s.x() - 4, s.y() - 4, 8, 8),
                       (m_grip.active && m_grip.id == id) ? kPreview : kGrip);
        }
    }
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
        // During a point-consuming command, clicks are input, not selection.
        if (m_processor && m_processor->hasActiveCommand()) {
            handleLeftClick(m_effectiveCursor, event->modifiers());
            return;
        }
        // Grip drag takes precedence over selection.
        if (const auto grip = gripAt(event->position())) {
            m_grip.active = true;
            m_grip.id = grip->first;
            m_grip.index = grip->second;
            return;
        }
        // Otherwise: pick or start a rubber band, decided on release/move.
        m_rubberBand = true;
        m_rubberStart = event->position();
    }
}

void CanvasWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_processor)
        m_processor->ctx().setPickTolerance(pickTolerance());
    m_cursorPx = event->position();
    if (m_panning) {
        m_camera.panPixels(event->position() - m_panLast);
        m_panLast = event->position();
        m_staticDirty = true;
    }
    m_effectiveCursor = effectivePoint(m_camera.screenToWorld(m_cursorPx));
    if (m_processor)
        m_processor->ctx().setPointerHint(m_effectiveCursor);
    emit cursorMoved(m_effectiveCursor.x, m_effectiveCursor.y);
    update();
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton && m_panning) {
        m_panning = false;
        setCursor(Qt::BlankCursor);
        return;
    }
    if (event->button() == Qt::LeftButton && m_grip.active) {
        m_grip.active = false;
        if (m_doc && m_doc->entity(m_grip.id)) {
            m_doc->beginTransaction(QStringLiteral("GRIP EDIT"));
            if (Entity* e = m_doc->beginModify(m_grip.id)) {
                e->moveGrip(m_grip.index, m_effectiveCursor);
                m_doc->endModify(m_grip.id);
            }
            m_doc->commitTransaction();
        }
        emit interaction();
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
    case InputKind::Number:
        m_processor->provideInput(InputValue::makePoint(world));
        break;
    case InputKind::EntitySet: {
        const EntityId id =
            hittest::pick(*m_doc, m_camera.screenToWorld(m_cursorPx), pickTolerance());
        if (id != kInvalidEntityId)
            m_processor->provideInput(InputValue::makeEntityRef(id));
        break;
    }
    case InputKind::Keyword:
        // Clicking at an options prompt accepts the default (like Enter),
        // so keyword stages (HATCH pattern/scale) are mouse-passable.
        m_processor->provideInput(InputValue::makeFinish());
        break;
    default:
        break;
    }
    emit interaction();
    // Repaint the static layer too: repeating commands (LINE, PLINE, POINT)
    // add entities inside a still-open transaction, which does not fire the
    // document-changed notification until the final Enter.
    markDocumentDirty();
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
        if (id != kInvalidEntityId) {
            // Locked layers: visible but not selectable.
            const Entity* e = m_doc->entity(id);
            const Layer* layer = e ? m_doc->layer(e->layerId()) : nullptr;
            if (!layer || !layer->locked)
                m_selection->toggle(id);
        }
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
        for (const EntityId id : ids) {
            const Entity* e = m_doc->entity(id);
            const Layer* layer = e ? m_doc->layer(e->layerId()) : nullptr;
            if ((!layer || (!layer->locked && layer->visible)))
                m_selection->add(id);
        }
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
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
        event->key() == Qt::Key_Space) {
        if (m_processor && m_processor->hasActiveCommand()) {
            m_processor->provideInput(InputValue::makeFinish());
            emit interaction();
            markDocumentDirty();
            return;
        }
        emit typed(QString()); // empty = repeat-last handled upstream
        return;
    }
    // Any printable character: hand off to the command bar.
    const QString text = event->text();
    if (!text.isEmpty() && text[0].isPrint()) {
        emit typed(text);
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace viki
