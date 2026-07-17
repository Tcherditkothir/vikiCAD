#include "PdfPlotter.h"

#include <QFont>
#include <QFontMetricsF>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPainterPath>
#include <QPdfWriter>

#include "doc/StickyNote.h"

namespace viki {

bool plotToPdf(const Document& doc, const Layout& layout, const QString& path,
               QString& error, bool includeNotes)
{
    QPdfWriter writer(path);
    writer.setPageSize(QPageSize(QSizeF(layout.paperW, layout.paperH),
                                 QPageSize::Millimeter, QString(),
                                 QPageSize::ExactMatch));
    writer.setPageMargins(QMarginsF(0, 0, 0, 0));
    writer.setResolution(1200);

    QPainter painter;
    if (!painter.begin(&writer)) {
        error = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    painter.setRenderHint(QPainter::Antialiasing);

    const double dotsPerMm = writer.resolution() / 25.4;
    const double penWidthDots = 0.35 * dotsPerMm; // fixed lineweight v1

    for (const Viewport& vp : layout.viewports) {
        painter.save();
        // Paper mm (origin bottom-left) -> device dots (origin top-left).
        const QRectF clipDots(vp.x * dotsPerMm,
                              (layout.paperH - vp.y - vp.h) * dotsPerMm,
                              vp.w * dotsPerMm, vp.h * dotsPerMm);
        painter.setClipRect(clipDots);

        const Vec2d paperCenter{vp.x + vp.w / 2.0, vp.y + vp.h / 2.0};
        const auto toDots = [&](const Vec2d& world) {
            const Vec2d paper = paperCenter + (world - vp.center) * vp.scale;
            return QPointF(paper.x * dotsPerMm, (layout.paperH - paper.y) * dotsPerMm);
        };

        RenderContext ctx;
        ctx.chordTolerance = 0.05 / vp.scale; // 0.05 mm on paper
        ctx.pixelSize = (1.0 / dotsPerMm) / vp.scale;
        ctx.doc = &doc;
        const Vec2d halfSpan{vp.w / (2 * vp.scale), vp.h / (2 * vp.scale)};
        ctx.viewBox = BBox2d(vp.center - halfSpan, vp.center + halfSpan);

        for (const EntityId id : doc.drawOrder()) {
            const Entity* e = doc.entity(id);
            if (!e)
                continue;
            const Layer* layer = doc.layer(e->layerId());
            if (layer && (!layer->visible || (!layer->printable && !includeNotes)))
                continue;
            if (!includeNotes && dynamic_cast<const StickyNoteEntity*>(e))
                continue;
            if (!doc.entityBounds(*e).intersects(ctx.viewBox))
                continue;
            ctx.resolvedColor = 0x000000; // black-on-white plot v1
            PrimitiveList list;
            e->buildPrimitives(ctx, list);

            for (const StrokePrimitive& s : list.strokes) {
                if (s.points.size() < 2)
                    continue;
                QPainterPath qp(toDots(s.points.front()));
                for (size_t i = 1; i < s.points.size(); ++i)
                    qp.lineTo(toDots(s.points[i]));
                if (s.closed)
                    qp.closeSubpath();
                if (s.filled) {
                    painter.fillPath(qp, Qt::black);
                } else if (s.width > 0.0) {
                    // World-width stroke: round caps/joins (Gerber trace).
                    painter.setPen(QPen(Qt::black, s.width * vp.scale * dotsPerMm,
                                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                    painter.drawPath(qp);
                } else {
                    painter.setPen(QPen(Qt::black, penWidthDots));
                    painter.drawPath(qp);
                }
            }
            for (const TextPrimitive& t : list.texts) {
                QFont font(QStringLiteral("DejaVu Sans"));
                font.setPixelSize(
                    std::max(1, int(std::lround(t.height * vp.scale * dotsPerMm))));
                painter.setFont(font);
                painter.setPen(QPen(Qt::black, penWidthDots));
                painter.save();
                painter.translate(toDots(t.pos));
                painter.rotate(-t.rotation * 180.0 / M_PI);
                double dx = 0;
                const double tw = QFontMetricsF(font).horizontalAdvance(t.text);
                if (t.hAlign == TextHAlign::Center)
                    dx = -tw / 2;
                else if (t.hAlign == TextHAlign::Right)
                    dx = -tw;
                painter.drawText(QPointF(dx, 0), t.text);
                painter.restore();
            }
        }

        // Viewport frame.
        painter.setClipping(false);
        painter.setPen(QPen(Qt::black, penWidthDots * 0.7));
        painter.drawRect(clipDots);
        painter.restore();
    }

    painter.end();
    return true;
}

} // namespace viki
