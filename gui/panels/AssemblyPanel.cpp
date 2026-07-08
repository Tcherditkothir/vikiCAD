#include "AssemblyPanel.h"

#include <map>

#include <QHeaderView>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "solid/SolidEntity.h"

namespace viki {

AssemblyPanel::AssemblyPanel(QWidget* parent) : QWidget(parent)
{
    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(1);
    m_tree->setHeaderLabels({QStringLiteral("Components")});
    m_tree->header()->setStretchLastSection(true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->addWidget(m_tree);
    connect(m_tree, &QTreeWidget::itemClicked, this, &AssemblyPanel::itemActivated);
}

void AssemblyPanel::attach(Document* doc, SelectionSet* selection)
{
    m_doc = doc;
    m_selection = selection;
    refresh();
}

void AssemblyPanel::refresh()
{
    m_tree->clear();
    if (!m_doc)
        return;
    // Group solids by component name (preserving insertion order per group).
    std::map<QString, std::vector<EntityId>> groups;
    std::vector<QString> order;
    for (const EntityId id : m_doc->drawOrder()) {
        const auto* s = dynamic_cast<const SolidEntity*>(m_doc->entity(id));
        if (!s)
            continue;
        const QString name =
            s->component.isEmpty() ? QStringLiteral("(ungrouped)") : s->component;
        if (groups.find(name) == groups.end())
            order.push_back(name);
        groups[name].push_back(id);
    }
    for (const QString& name : order) {
        const auto& ids = groups[name];
        auto* top = new QTreeWidgetItem(
            m_tree, {QStringLiteral("%1  (%2)").arg(name).arg(ids.size())});
        top->setData(0, Qt::UserRole, QVariant());     // group marker
        top->setData(0, Qt::UserRole + 1, name);
        for (size_t i = 0; i < ids.size(); ++i) {
            auto* child = new QTreeWidgetItem(
                top, {QStringLiteral("solid #%1").arg(qlonglong(ids[i]))});
            child->setData(0, Qt::UserRole, qlonglong(ids[i]));
        }
    }
    m_tree->expandAll();
}

void AssemblyPanel::itemActivated(QTreeWidgetItem* item, int)
{
    if (!m_doc || !m_selection || !item)
        return;
    m_selection->clear();
    const QVariant idVar = item->data(0, Qt::UserRole);
    if (idVar.isValid()) {
        m_selection->toggle(EntityId(idVar.toLongLong())); // a single solid
    } else {
        // A component group: select every solid under it.
        for (int i = 0; i < item->childCount(); ++i) {
            const QVariant childId = item->child(i)->data(0, Qt::UserRole);
            if (childId.isValid())
                m_selection->toggle(EntityId(childId.toLongLong()));
        }
    }
    emit selectionChanged();
}

} // namespace viki
