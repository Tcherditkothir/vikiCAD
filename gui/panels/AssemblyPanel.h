#pragma once

#include <QWidget>

#include "doc/Document.h"
#include "doc/SelectionSet.h"

class QTreeWidget;
class QTreeWidgetItem;

namespace viki {

// Assembly tree: solids grouped by component name. Selecting rows (multi-select
// with Ctrl/Shift) drives the document selection, which highlights the solids
// in the 2D and 3D views and feeds the Properties panel. The context menu acts
// on the whole selection: Combine (join), Move, Color, Transparency, Rename,
// Delete.
class AssemblyPanel : public QWidget {
    Q_OBJECT
public:
    explicit AssemblyPanel(QWidget* parent = nullptr);

    void attach(Document* doc, SelectionSet* selection);
    void refresh();

signals:
    void selectionChanged(); // ask the host to repaint / sync the 3D view
    // Run a command line through the shared processor (COMBINE, MOVE3D…): the
    // host submits it exactly as if typed, so undo/refresh come for free.
    void commandRequested(const QString& line);

private:
    void syncSelectionFromTree();
    void showContextMenu(const QPoint& pos);
    std::vector<EntityId> idsForItem(QTreeWidgetItem* item) const;
    std::vector<EntityId> selectedIds() const;

    QTreeWidget* m_tree = nullptr;
    Document* m_doc = nullptr;
    SelectionSet* m_selection = nullptr;
};

} // namespace viki
