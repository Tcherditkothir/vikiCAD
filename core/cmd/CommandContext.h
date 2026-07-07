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
};

// Everything a command needs to run, front-end agnostic.
class CommandContext {
public:
    CommandContext(Document& doc, SelectionSet& selection, ViewHook* view = nullptr)
        : m_doc(doc), m_selection(selection), m_view(view) {}

    Document& doc() { return m_doc; }
    SelectionSet& selection() { return m_selection; }
    ViewHook* view() { return m_view; }

    // Base point for relative coordinates (@dx,dy) and Distance-by-point.
    Vec2d lastPoint() const { return m_lastPoint; }
    void setLastPoint(const Vec2d& p) { m_lastPoint = p; }

    // Messages surfaced to the user (command bar history / CLI output).
    void info(const QString& msg) { m_messages.push_back(msg); }
    const std::vector<QString>& messages() const { return m_messages; }
    void clearMessages() { m_messages.clear(); }

private:
    Document& m_doc;
    SelectionSet& m_selection;
    ViewHook* m_view;
    Vec2d m_lastPoint;
    std::vector<QString> m_messages;
};

} // namespace viki
