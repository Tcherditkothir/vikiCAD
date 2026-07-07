#pragma once

#include <QPointF>
#include <QSize>

#include "geom/BBox2d.h"

namespace viki {

// World (mm, y up) <-> screen (px, y down) mapping.
class Camera2d {
public:
    double scale() const { return m_scale; } // px per mm
    Vec2d center() const { return m_center; }
    void setViewport(const QSize& sizePx) { m_viewport = sizePx; }

    QPointF worldToScreen(const Vec2d& w) const
    {
        return {m_viewport.width() * 0.5 + (w.x - m_center.x) * m_scale,
                m_viewport.height() * 0.5 - (w.y - m_center.y) * m_scale};
    }
    Vec2d screenToWorld(const QPointF& s) const
    {
        return {m_center.x + (s.x() - m_viewport.width() * 0.5) / m_scale,
                m_center.y - (s.y() - m_viewport.height() * 0.5) / m_scale};
    }
    double pixelsToWorld(double px) const { return px / m_scale; }

    void panPixels(const QPointF& deltaPx)
    {
        m_center.x -= deltaPx.x() / m_scale;
        m_center.y += deltaPx.y() / m_scale;
    }
    // Zoom by `factor`, keeping the world point under `anchorPx` fixed.
    void zoomAt(const QPointF& anchorPx, double factor)
    {
        const Vec2d before = screenToWorld(anchorPx);
        m_scale = std::clamp(m_scale * factor, 1e-6, 1e6);
        const Vec2d after = screenToWorld(anchorPx);
        m_center += before - after;
    }
    void zoomExtents(const BBox2d& box)
    {
        if (!box.isValid() || m_viewport.isEmpty()) {
            m_center = {};
            m_scale = 4.0;
            return;
        }
        m_center = box.center();
        const double w = std::max(box.width(), 1e-6);
        const double h = std::max(box.height(), 1e-6);
        m_scale = 0.9 * std::min(m_viewport.width() / w, m_viewport.height() / h);
        m_scale = std::clamp(m_scale, 1e-6, 1e6);
    }

private:
    Vec2d m_center;
    double m_scale = 4.0; // sensible startup: ~250 mm across a 1000 px window
    QSize m_viewport{1, 1};
};

} // namespace viki
