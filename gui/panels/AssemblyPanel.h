#pragma once

#include <QWidget>

#include "doc/Document.h"
#include "doc/SelectionSet.h"

class QTreeWidget;
class QTreeWidgetItem;

namespace viki {

// Assembly tree: solids grouped by component name. Clicking a component (or a
// single solid) selects it, which highlights it in the 2D and 3D views.
class AssemblyPanel : public QWidget {
    Q_OBJECT
public:
    explicit AssemblyPanel(QWidget* parent = nullptr);

    void attach(Document* doc, SelectionSet* selection);
    void refresh();

signals:
    void selectionChanged(); // ask the host to repaint / sync the 3D view

private:
    void itemActivated(QTreeWidgetItem* item, int column);
    void showContextMenu(const QPoint& pos);
    std::vector<EntityId> idsForItem(QTreeWidgetItem* item) const;

    QTreeWidget* m_tree = nullptr;
    Document* m_doc = nullptr;
    SelectionSet* m_selection = nullptr;
};

} // namespace viki
