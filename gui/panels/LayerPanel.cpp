#include "LayerPanel.h"

#include <QColorDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace viki {

namespace {
enum Col { ColCurrent = 0, ColName, ColColor, ColVisible, ColLocked, ColCount };
}

LayerPanel::LayerPanel(QWidget* parent)
    : QWidget(parent)
{
    m_table = new QTableWidget(this);
    m_table->setColumnCount(ColCount);
    m_table->setHorizontalHeaderLabels(
        {QStringLiteral(""), QStringLiteral("Name"), QStringLiteral("Color"),
         QStringLiteral("On"), QStringLiteral("Lock")});
    m_table->horizontalHeader()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    // Click a header to sort (by name, color, On/Lock state). Row->layer
    // lookups go through the item's stored id, so sorting is safe.
    m_table->setSortingEnabled(true);
    m_table->horizontalHeader()->setSectionsClickable(true);
    m_table->sortByColumn(ColName, Qt::AscendingOrder);

    auto* addBtn = new QPushButton(QStringLiteral("+"), this);
    auto* delBtn = new QPushButton(QStringLiteral("−"), this);
    addBtn->setFixedWidth(28);
    delBtn->setFixedWidth(28);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(addBtn);
    buttons->addWidget(delBtn);
    buttons->addStretch(1);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->addWidget(m_table, 1);
    layout->addLayout(buttons);

    connect(addBtn, &QPushButton::clicked, this, &LayerPanel::addLayerClicked);
    connect(delBtn, &QPushButton::clicked, this, &LayerPanel::removeLayerClicked);
    connect(m_table, &QTableWidget::cellChanged, this, &LayerPanel::cellChanged);
    connect(m_table, &QTableWidget::cellClicked, this, &LayerPanel::cellClicked);
}

void LayerPanel::attach(Document* doc)
{
    m_doc = doc;
    refresh();
}

void LayerPanel::refresh()
{
    if (!m_doc)
        return;
    m_refreshing = true;
    const bool wasSorted = m_table->isSortingEnabled();
    m_table->setSortingEnabled(false); // populate by index, then re-sort
    const auto& layers = m_doc->layers();
    m_table->setRowCount(int(layers.size()));
    for (int row = 0; row < int(layers.size()); ++row) {
        const Layer& l = layers[size_t(row)];

        auto* current = new QTableWidgetItem;
        current->setFlags(Qt::ItemIsEnabled);
        current->setText(l.id == m_doc->currentLayer() ? QStringLiteral("✔") : QString());
        current->setTextAlignment(Qt::AlignCenter);
        current->setData(Qt::UserRole, qlonglong(l.id));
        m_table->setItem(row, ColCurrent, current);

        auto* name = new QTableWidgetItem(l.name);
        if (l.id == 0)
            name->setFlags(name->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(row, ColName, name);

        auto* color = new QTableWidgetItem;
        color->setFlags(Qt::ItemIsEnabled);
        color->setBackground(QColor::fromRgb(l.rgb));
        m_table->setItem(row, ColColor, color);

        auto* visible = new QTableWidgetItem;
        visible->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        visible->setCheckState(l.visible ? Qt::Checked : Qt::Unchecked);
        m_table->setItem(row, ColVisible, visible);

        auto* locked = new QTableWidgetItem;
        locked->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        locked->setCheckState(l.locked ? Qt::Checked : Qt::Unchecked);
        m_table->setItem(row, ColLocked, locked);
    }
    m_table->setSortingEnabled(wasSorted);
    m_refreshing = false;
}

void LayerPanel::addLayerClicked()
{
    if (!m_doc)
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("New layer"),
                                               QStringLiteral("Layer name:"),
                                               QLineEdit::Normal, {}, &ok);
    if (!ok)
        return; // user cancelled
    if (name.trimmed().isEmpty()) {
        QMessageBox::information(this, QStringLiteral("No name"),
                                 QStringLiteral("Layer name cannot be empty."));
        return;
    }
    if (m_doc->layerByName(name)) {
        QMessageBox::information(this, QStringLiteral("Layer exists"),
                                 QStringLiteral("A layer named '%1' already exists.").arg(name));
        return;
    }
    m_doc->ensureLayer(name.trimmed());
    refresh();
    emit layersChanged();
}

void LayerPanel::removeLayerClicked()
{
    if (!m_doc)
        return;
    const int row = m_table->currentRow();
    if (row < 0) {
        QMessageBox::information(
            this, QStringLiteral("No layer selected"),
            QStringLiteral("Click a layer in the list first, then −."));
        return;
    }
    const LayerId id = m_table->item(row, ColCurrent)->data(Qt::UserRole).toLongLong();
    if (!m_doc->removeLayer(id)) {
        QMessageBox::information(
            this, QStringLiteral("Cannot remove"),
            QStringLiteral("Layer 0, the current layer, and non-empty layers "
                           "cannot be removed."));
        return;
    }
    refresh();
    emit layersChanged();
}

void LayerPanel::cellClicked(int row, int column)
{
    if (!m_doc || m_refreshing)
        return;
    const LayerId id = m_table->item(row, ColCurrent)->data(Qt::UserRole).toLongLong();
    const Layer* l = m_doc->layer(id);
    if (!l)
        return;

    if (column == ColCurrent) {
        m_doc->setCurrentLayer(id);
        refresh();
        emit layersChanged();
    } else if (column == ColColor) {
        const QColor c =
            QColorDialog::getColor(QColor::fromRgb(l->rgb), this, QStringLiteral("Layer color"));
        if (c.isValid()) {
            m_doc->setLayerProps(id, c.rgb() & 0xFFFFFF, l->visible, l->locked);
            refresh();
            emit layersChanged();
        }
    }
}

void LayerPanel::cellChanged(int row, int column)
{
    if (!m_doc || m_refreshing)
        return;
    const LayerId id = m_table->item(row, ColCurrent)->data(Qt::UserRole).toLongLong();
    Layer* l = nullptr;
    for (const Layer& candidate : m_doc->layers())
        if (candidate.id == id)
            l = const_cast<Layer*>(&candidate);
    if (!l)
        return;

    if (column == ColVisible || column == ColLocked) {
        const bool visible = m_table->item(row, ColVisible)->checkState() == Qt::Checked;
        const bool locked = m_table->item(row, ColLocked)->checkState() == Qt::Checked;
        m_doc->setLayerProps(id, l->rgb, visible, locked);
        emit layersChanged();
    } else if (column == ColName && id != 0) {
        const QString newName = m_table->item(row, ColName)->text().trimmed();
        if (newName == l->name) {
            refresh(); // no-op rename (or whitespace trim): just redisplay
        } else if (newName.isEmpty()) {
            refresh(); // revert display BEFORE the modal box re-fires signals
            QMessageBox::information(
                this, QStringLiteral("Rename cancelled"),
                QStringLiteral("Layer name cannot be empty."));
        } else if (m_doc->layerByName(newName)) {
            refresh();
            QMessageBox::information(
                this, QStringLiteral("Rename cancelled"),
                QStringLiteral("A layer named '%1' already exists.").arg(newName));
        } else {
            l->name = newName;
            refresh();
        }
        emit layersChanged();
    }
}

} // namespace viki
