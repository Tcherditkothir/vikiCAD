#pragma once

#include <QString>

#include "doc/Document.h"
#include "doc/SelectionSet.h"

namespace viki {

// View operations a command may request; null hooks are no-ops (headless).
class ViewHook {
public:
    virtual ~ViewHook() = default;
    virtual void zoomExtents() = 0;
    virtual void zoomWindow(const BBox2d& box) { (void)box; }
};

// Everything a command needs to run, front-end agnostic.
class CommandContext {
public:
    CommandContext(Document& doc, SelectionSet& selection, ViewHook* view = nullptr)
        : m_doc(doc), m_selection(selection), m_view(view) {}

    Document& doc() { return m_doc; }
    SelectionSet& selection() { return m_selection; }
    ViewHook* view() { return m_view; }

    // mm per display unit — input parsing multiplies bare numbers by this.
    double unitFactor() const
    {
        return m_doc.displayUnits() == DisplayUnits::Inches ? 25.4 : 1.0;
    }

    // Base point for relative coordinates (@dx,dy) and Distance-by-point.
    Vec2d lastPoint() const { return m_lastPoint; }
    void setLastPoint(const Vec2d& p) { m_lastPoint = p; }

    // Live cursor position (GUI): enables AutoCAD-style direct distance
    // entry — a bare number at a Point prompt lands that far from lastPoint
    // toward the pointer.
    void setPointerHint(const Vec2d& p) { m_pointerHint = p; m_hasPointerHint = true; }
    bool pointerDirection(Vec2d& dirOut) const
    {
        if (!m_hasPointerHint)
            return false;
        const Vec2d d = m_pointerHint - m_lastPoint;
        if (d.lengthSq() < kGeomTol)
            return false;
        dirOut = d.normalized();
        return true;
    }

    // World tolerance for point-driven entity picks (TRIM target, etc.).
    // The GUI sets it from the zoom; headless falls back to a size-relative one.
    void setPickTolerance(double tol) { m_pickTolerance = tol; }
    double pickTolerance() const
    {
        if (m_pickTolerance > 0)
            return m_pickTolerance;
        const BBox2d ext = m_doc.extents();
        return ext.isValid()
                   ? std::max(1e-3, 1e-3 * std::max(ext.width(), ext.height()))
                   : 1e-3;
    }

    // Messages surfaced to the user (command bar history / CLI output).
    void info(const QString& msg) { m_messages.push_back(msg); }
    const std::vector<QString>& messages() const { return m_messages; }
    void clearMessages() { m_messages.clear(); }

private:
    Document& m_doc;
    SelectionSet& m_selection;
    ViewHook* m_view;
    Vec2d m_lastPoint;
    Vec2d m_pointerHint;
    bool m_hasPointerHint = false;
    double m_pickTolerance = 0.0;
    std::vector<QString> m_messages;
};

} // namespace viki
