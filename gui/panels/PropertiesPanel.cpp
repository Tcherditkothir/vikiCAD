#include "PropertiesPanel.h"

#include <QColorDialog>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>

namespace viki {

PropertiesPanel::PropertiesPanel(QWidget* parent)
    : QWidget(parent)
{
    m_countLabel = new QLabel(QStringLiteral("No selection"), this);
    m_layerCombo = new QComboBox(this);
    m_colorBtn = new QPushButton(QStringLiteral("Pick..."), this);
    m_byLayerBtn = new QPushButton(QStringLiteral("ByLayer"), this);

    auto* colorRow = new QWidget(this);
    auto* colorLayout = new QHBoxLayout(colorRow);
    colorLayout->setContentsMargins(0, 0, 0, 0);
    colorLayout->addWidget(m_colorBtn, 1);
    colorLayout->addWidget(m_byLayerBtn, 1);

    auto* form = new QFormLayout(this);
    form->setContentsMargins(6, 6, 6, 6);
    form->addRow(m_countLabel);
    form->addRow(QStringLiteral("Layer:"), m_layerCombo);
    form->addRow(QStringLiteral("Color:"), colorRow);

    connect(m_layerCombo, &QComboBox::activated, this, &PropertiesPanel::layerPicked);
    connect(m_colorBtn, &QPushButton::clicked, this, &PropertiesPanel::colorClicked);
    connect(m_byLayerBtn, &QPushButton::clicked, this, &PropertiesPanel::byLayerClicked);
}

void PropertiesPanel::attach(Document* doc, SelectionSet* selection)
{
    m_doc = doc;
    m_selection = selection;
    refresh();
}

void PropertiesPanel::refresh()
{
    if (!m_doc || !m_selection)
        return;
    m_refreshing = true;

    m_layerCombo->clear();
    for (const Layer& l : m_doc->layers())
        m_layerCombo->addItem(l.name, qlonglong(l.id));

    const size_t n = m_selection->size();
    const bool has = n > 0;
    m_countLabel->setText(has ? QStringLiteral("%1 selected").arg(n)
                              : QStringLiteral("No selection"));
    m_layerCombo->setEnabled(has);
    m_colorBtn->setEnabled(has);
    m_byLayerBtn->setEnabled(has);

    if (has) {
        // Show the common layer if the whole selection shares one.
        LayerId common = -1;
        bool same = true;
        for (const EntityId id : m_selection->ids()) {
            const Entity* e = m_doc->entity(id);
            if (!e)
                continue;
            if (common == -1)
                common = e->layerId();
            else
                same = same && (common == e->layerId());
        }
        if (same && common >= 0)
            m_layerCombo->setCurrentIndex(m_layerCombo->findData(qlonglong(common)));
        else
            m_layerCombo->setCurrentIndex(-1);
    }
    m_refreshing = false;
}

void PropertiesPanel::applyToSelection(const std::function<void(Entity&)>& fn,
                                       const QString& txName)
{
    if (!m_doc || !m_selection || m_selection->isEmpty())
        return;
    m_doc->beginTransaction(txName);
    for (const EntityId id : m_selection->ids()) {
        if (Entity* e = m_doc->beginModify(id)) {
            fn(*e);
            m_doc->endModify(id);
        }
    }
    m_doc->commitTransaction();
    emit propertiesApplied();
}

void PropertiesPanel::layerPicked(int index)
{
    if (m_refreshing || index < 0)
        return;
    const LayerId layer = m_layerCombo->itemData(index).toLongLong();
    applyToSelection([layer](Entity& e) { e.setLayerId(layer); },
                     QStringLiteral("LAYER CHANGE"));
}

void PropertiesPanel::colorClicked()
{
    if (m_refreshing)
        return;
    const QColor c = QColorDialog::getColor(Qt::white, this, QStringLiteral("Entity color"));
    if (!c.isValid())
        return;
    ColorSpec spec;
    spec.byLayer = false;
    spec.rgb = c.rgb() & 0xFFFFFF;
    applyToSelection([spec](Entity& e) { e.setColor(spec); }, QStringLiteral("COLOR CHANGE"));
}

void PropertiesPanel::byLayerClicked()
{
    if (m_refreshing)
        return;
    applyToSelection([](Entity& e) { e.setColor(ColorSpec{}); },
                     QStringLiteral("COLOR BYLAYER"));
}

} // namespace viki
