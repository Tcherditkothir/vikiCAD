#include "AssemblyPanel.h"

#include <algorithm>
#include <functional>
#include <map>

#include <QColorDialog>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
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
    // Ctrl/Shift multi-select: pick several solids, then right-click to
    // Combine / Move / … the whole set.
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->addWidget(m_tree);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this,
            &AssemblyPanel::syncSelectionFromTree);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this,
            &AssemblyPanel::showContextMenu);
}

std::vector<EntityId> AssemblyPanel::idsForItem(QTreeWidgetItem* item) const
{
    std::vector<EntityId> ids;
    if (!item)
        return ids;
    const QVariant idVar = item->data(0, Qt::UserRole);
    if (idVar.isValid()) {
        ids.push_back(EntityId(idVar.toLongLong())); // single solid
    } else {
        for (int i = 0; i < item->childCount(); ++i) {
            const QVariant cid = item->child(i)->data(0, Qt::UserRole);
            if (cid.isValid())
                ids.push_back(EntityId(cid.toLongLong()));
        }
    }
    return ids;
}

// Every solid id currently selected in the tree (groups expand to their
// children), deduplicated, in tree order.
std::vector<EntityId> AssemblyPanel::selectedIds() const
{
    std::vector<EntityId> ids;
    for (QTreeWidgetItem* item : m_tree->selectedItems())
        for (const EntityId id : idsForItem(item))
            if (std::find(ids.begin(), ids.end(), id) == ids.end())
                ids.push_back(id);
    return ids;
}

void AssemblyPanel::showContextMenu(const QPoint& pos)
{
    if (!m_doc)
        return;
    // Act on the multi-selection; fall back to the row under the cursor.
    std::vector<EntityId> ids = selectedIds();
    if (ids.empty())
        ids = idsForItem(m_tree->itemAt(pos));
    if (ids.empty())
        return;

    // Apply fn to every target solid in one undoable transaction.
    const auto forEachSolid = [&](const QString& tx,
                                  const std::function<void(SolidEntity&)>& fn) {
        m_doc->beginTransaction(tx);
        for (const EntityId id : ids)
            if (dynamic_cast<const SolidEntity*>(m_doc->entity(id)))
                if (auto* s = dynamic_cast<SolidEntity*>(m_doc->beginModify(id))) {
                    fn(*s);
                    m_doc->endModify(id);
                }
        m_doc->commitTransaction();
        refresh();
        emit selectionChanged();
    };

    QMenu menu(this);
    connect(menu.addAction(QStringLiteral("Properties")), &QAction::triggered,
            this, [this] { syncSelectionFromTree(); }); // select -> panel

    // Boolean-join the whole selection into one body (COMBINE command: same
    // code path as typing it, so undo and messages come for free).
    if (ids.size() >= 2) {
        connect(menu.addAction(QStringLiteral("Combine (join) %1 solids")
                                   .arg(ids.size())),
                &QAction::triggered, this, [this, ids] {
                    QStringList line{QStringLiteral("COMBINE")};
                    for (const EntityId id : ids)
                        line << QString::number(qlonglong(id));
                    emit commandRequested(line.join(QLatin1Char(' ')));
                });
    }

    connect(menu.addAction(QStringLiteral("Move…")), &QAction::triggered, this,
            [this, ids] {
                bool ok = false;
                const QString text = QInputDialog::getText(
                    this, QStringLiteral("Move solids"),
                    QStringLiteral("Displacement dx, dy, dz (mm):"),
                    QLineEdit::Normal, QStringLiteral("0, 0, 0"), &ok);
                if (!ok)
                    return;
                const QStringList parts =
                    text.split(QLatin1Char(','), Qt::SkipEmptyParts);
                if (parts.size() != 3)
                    return;
                bool okx = false, oky = false, okz = false;
                const double dx = parts[0].trimmed().toDouble(&okx);
                const double dy = parts[1].trimmed().toDouble(&oky);
                const double dz = parts[2].trimmed().toDouble(&okz);
                if (!okx || !oky || !okz)
                    return;
                QStringList line{QStringLiteral("MOVE3D"), QString::number(dx),
                                 QString::number(dy), QString::number(dz)};
                for (const EntityId id : ids)
                    line << QString::number(qlonglong(id));
                emit commandRequested(line.join(QLatin1Char(' ')));
            });

    const SolidEntity* first =
        dynamic_cast<const SolidEntity*>(m_doc->entity(ids.front()));

    connect(menu.addAction(QStringLiteral("Color…")), &QAction::triggered, this,
            [this, forEachSolid, first] {
                const uint32_t cur = first ? m_doc->resolveColor(*first) : 0xCCCCCC;
                const QColor c = QColorDialog::getColor(
                    QColor::fromRgb(cur), this, QStringLiteral("Component color"));
                if (c.isValid())
                    forEachSolid(QStringLiteral("COLOR"), [&](SolidEntity& s) {
                        s.setColor({false, uint32_t(c.rgb() & 0xFFFFFF)});
                    });
            });

    connect(menu.addAction(QStringLiteral("Transparency…")), &QAction::triggered,
            this, [this, forEachSolid, first] {
                bool ok = false;
                const int cur = first ? int(first->transparency * 100) : 0;
                const int pct = QInputDialog::getInt(
                    this, QStringLiteral("Transparency"),
                    QStringLiteral("Transparency %:"), cur, 0, 95, 5, &ok);
                if (ok)
                    forEachSolid(QStringLiteral("TRANSPARENCY"),
                                 [&](SolidEntity& s) { s.transparency = pct / 100.0; });
            });

    connect(menu.addAction(QStringLiteral("Rename component…")),
            &QAction::triggered, this, [this, forEachSolid, first] {
                bool ok = false;
                const QString name = QInputDialog::getText(
                    this, QStringLiteral("Rename component"),
                    QStringLiteral("Component name:"), QLineEdit::Normal,
                    first ? first->component : QString(), &ok);
                if (ok)
                    forEachSolid(QStringLiteral("RENAME"),
                                 [&](SolidEntity& s) { s.component = name.trimmed(); });
            });

    menu.addSeparator();
    connect(menu.addAction(QStringLiteral("Delete")), &QAction::triggered, this,
            [this, ids] {
                m_doc->beginTransaction(QStringLiteral("DELETE"));
                for (const EntityId id : ids)
                    m_doc->removeEntity(id);
                m_doc->commitTransaction();
                refresh();
                emit selectionChanged();
            });

    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void AssemblyPanel::attach(Document* doc, SelectionSet* selection)
{
    m_doc = doc;
    m_selection = selection;
    refresh();
}

void AssemblyPanel::refresh()
{
    // Rebuilding the tree must not clobber the document selection (clear()
    // would fire itemSelectionChanged -> sync -> empty selection).
    const QSignalBlocker blocker(m_tree);
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
            // Reflect the document selection (e.g. a solid picked in the 3D
            // view) back into the tree.
            if (m_selection && m_selection->contains(ids[i]))
                child->setSelected(true);
        }
    }
    m_tree->expandAll();
}

// Mirror the tree's (multi-)selection into the document selection: the 2D/3D
// views highlight it and the Properties panel binds to it.
void AssemblyPanel::syncSelectionFromTree()
{
    if (!m_doc || !m_selection)
        return;
    m_selection->clear();
    for (const EntityId id : selectedIds())
        m_selection->add(id);
    emit selectionChanged();
}

} // namespace viki
