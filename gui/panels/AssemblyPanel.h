#pragma once

#include <QWidget>

#include "doc/Document.h"
#include "doc/SelectionSet.h"

class QTreeWidget;
class QTreeWidgetItem;

namespace viki {

// Assembly tree: solids grouped by component name, plus a top-level
// "Sketches" group (one row per sketch: name + entity count). Selecting rows
// (multi-select with Ctrl/Shift) drives the document selection, which
// highlights the solids/sketch entities in the 2D and 3D views and feeds the
// Properties panel. The context menu acts on the whole selection: Combine
// (join), Move, Color, Transparency, Rename, Delete. Sketch rows get their
// own menu: Open (SKETCH Open through the shared processor + switch to 2D),
// Rename, Delete (keeps the entities, drops their sketch tag).
class AssemblyPanel : public QWidget {
    Q_OBJECT
public:
    explicit AssemblyPanel(QWidget* parent = nullptr);

    void attach(Document* doc, SelectionSet* selection);
    void refresh();
    // Highlight a feature row (e.g. the hole whose wall was clicked in 3D).
    void focusFeature(EntityId solid, int nodeIndex);

signals:
    void selectionChanged(); // ask the host to repaint / sync the 3D view
    // Run a command line through the shared processor (COMBINE, MOVE3D…): the
    // host submits it exactly as if typed, so undo/refresh come for free.
    void commandRequested(const QString& line);
    // A feature row (hole/shell/…) was selected in the tree.
    void featureFocused(EntityId solid, int nodeIndex);
    // One-line user feedback (e.g. why a context-menu action was refused);
    // routed to the command bar history by the host.
    void feedback(const QString& message);
    // A sketch was opened from the panel: the host switches to the 2D view
    // (the canvas IS the sketch plane).
    void sketchActivated();

private:
    void syncSelectionFromTree();
    void showContextMenu(const QPoint& pos);
    void openSketch(int64_t id);
    // Sketch id of a tree row, or 0 if the row is not a sketch.
    static int64_t sketchIdForItem(QTreeWidgetItem* item);
    std::vector<EntityId> idsForItem(QTreeWidgetItem* item) const;
    std::vector<EntityId> selectedIds() const;

    QTreeWidget* m_tree = nullptr;
    Document* m_doc = nullptr;
    SelectionSet* m_selection = nullptr;
};

} // namespace viki
