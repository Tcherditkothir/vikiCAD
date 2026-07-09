#include "PropertiesPanel.h"

#include <cmath>

#include <QColorDialog>
#include <QComboBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>

#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include "solid/SolidEntity.h"

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

    m_geomTable = new QTableWidget(this);
    m_geomTable->setColumnCount(2);
    m_geomTable->setHorizontalHeaderLabels({QStringLiteral("Prop"), QStringLiteral("Value")});
    m_geomTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_geomTable->verticalHeader()->setVisible(false);
    m_geomTable->setMinimumHeight(160);

    auto* form = new QFormLayout(this);
    form->setContentsMargins(6, 6, 6, 6);
    form->addRow(m_countLabel);
    form->addRow(QStringLiteral("Layer:"), m_layerCombo);
    form->addRow(QStringLiteral("Color:"), colorRow);
    form->addRow(m_geomTable);

    connect(m_geomTable, &QTableWidget::cellChanged, this,
            &PropertiesPanel::geometryCellChanged);
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
    rebuildGeometryTable();
    m_refreshing = false;
}

namespace {
// Leaf JSON values we expose for editing; nested arrays (vertices, rings)
// show as read-only summaries.
bool isPointArray(const QJsonValue& v)
{
    return v.isArray() && v.toArray().size() == 2 && v.toArray().at(0).isDouble();
}
} // namespace

void PropertiesPanel::rebuildGeometryTable()
{
    m_geomTable->blockSignals(true);
    m_geomTable->setRowCount(0);
    m_featureParams.clear();
    m_featureRowStart = -1;
    m_originRowStart = -1;
    if (m_selection && m_selection->size() == 1 && m_doc) {
        const Entity* e = m_doc->entity(m_selection->ids().front());
        if (e) {
            const QJsonObject geom = e->toJson()[QStringLiteral("geom")].toObject();
            int row = 0;
            const auto addRow = [&](const QString& key, const QString& value,
                                    bool editable) {
                m_geomTable->insertRow(row);
                auto* k = new QTableWidgetItem(key);
                k->setFlags(Qt::ItemIsEnabled);
                m_geomTable->setItem(row, 0, k);
                auto* val = new QTableWidgetItem(value);
                if (!editable)
                    val->setFlags(Qt::ItemIsEnabled);
                m_geomTable->setItem(row, 1, val);
                ++row;
            };
            addRow(QStringLiteral("type"), QLatin1String(e->typeName()), false);
            addRow(QStringLiteral("id"), QString::number(e->id()), false);
            for (auto it = geom.begin(); it != geom.end(); ++it) {
                if (it.key() == QLatin1String("brep"))
                    continue; // huge base64 BREP blob — not human-editable
                const QJsonValue v = it.value();
                if (v.isDouble())
                    addRow(it.key(), QString::number(v.toDouble(), 'f', 4), true);
                else if (v.isString())
                    addRow(it.key(), v.toString(), true);
                else if (v.isBool())
                    addRow(it.key(), v.toBool() ? QStringLiteral("1")
                                                : QStringLiteral("0"), true);
                else if (isPointArray(v))
                    addRow(it.key(),
                           QStringLiteral("%1, %2")
                               .arg(v.toArray().at(0).toDouble(), 0, 'f', 4)
                               .arg(v.toArray().at(1).toDouble(), 0, 'f', 4),
                           true);
                else if (v.isArray())
                    addRow(it.key(),
                           QStringLiteral("(%1 items)").arg(v.toArray().size()), false);
            }
            // Feature parameters (Fusion parity): a solid with a parametric
            // history exposes each node's editable scalars, e.g.
            // "hole 2: diameter". Edited through the FeatureTree setters +
            // regenerateFeatures, not through the geom JSON.
            if (const auto* solid = dynamic_cast<const SolidEntity*>(e)) {
                // Where the solid IS: its bounding-box origin corner. Editing
                // a coordinate translates the solid there (typed move).
                m_originRowStart = row;
                addRow(QStringLiteral("origin x"),
                       QString::number(solid->bounds().min.x, 'f', 4), true);
                addRow(QStringLiteral("origin y"),
                       QString::number(solid->bounds().min.y, 'f', 4), true);
                addRow(QStringLiteral("origin z"),
                       QString::number(solid->zMin(), 'f', 4), true);
                if (solid->features && solid->features->count() > 0) {
                    m_featureParams = featureparams::list(*solid->features);
                    if (!m_featureParams.empty())
                        m_featureRowStart = row;
                    for (const auto& p : m_featureParams)
                        addRow(p.label, QString::number(p.value, 'f', 4), true);
                }
            }
        }
    }
    m_geomTable->blockSignals(false);
}

void PropertiesPanel::geometryCellChanged(int row, int column)
{
    if (m_refreshing || column != 1 || !m_doc || !m_selection ||
        m_selection->size() != 1)
        return;
    if (m_featureRowStart >= 0 && row >= m_featureRowStart) {
        applyFeatureEdit(row - m_featureRowStart);
        return;
    }
    if (m_originRowStart >= 0 && row >= m_originRowStart &&
        row < m_originRowStart + 3) {
        applyOriginEdit(row - m_originRowStart);
        return;
    }
    const EntityId id = m_selection->ids().front();
    Entity* probe = m_doc->entity(id);
    if (!probe)
        return;
    const QString key = m_geomTable->item(row, 0)->text();
    const QString text = m_geomTable->item(row, 1)->text().trimmed();

    QJsonObject full = probe->toJson();
    QJsonObject geom = full[QStringLiteral("geom")].toObject();
    if (!geom.contains(key))
        return;
    const QJsonValue old = geom[key];
    QJsonValue next = old;
    bool ok = false;
    if (old.isDouble()) {
        const double d = text.toDouble(&ok);
        if (ok)
            next = d;
    } else if (old.isString()) {
        QString content = text;
        content.replace(QLatin1String("\\n"), QLatin1String("\n"));
        next = content;
        ok = true;
    } else if (old.isBool()) {
        next = (text == QLatin1String("1") ||
                text.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0);
        ok = true;
    } else if (isPointArray(old)) {
        const QStringList parts = text.split(QLatin1Char(','));
        if (parts.size() == 2) {
            bool okX = false, okY = false;
            const double x = parts[0].trimmed().toDouble(&okX);
            const double y = parts[1].trimmed().toDouble(&okY);
            if (okX && okY) {
                next = QJsonArray{x, y};
                ok = true;
            }
        }
    }
    if (!ok) {
        rebuildGeometryTable(); // revert display
        return;
    }
    geom[key] = next;
    full[QStringLiteral("geom")] = geom;

    m_doc->beginTransaction(QStringLiteral("PROPERTIES"));
    if (Entity* e = m_doc->beginModify(id)) {
        e->fromJson(full);
        m_doc->endModify(id);
    }
    m_doc->commitTransaction();
    emit propertiesApplied();
    rebuildGeometryTable();
}

// One feature-parameter edit: set the param via the FeatureTree setter and
// replay the history, all inside a single journaled transaction. If the
// regeneration fails the param is reverted and the transaction rolled back —
// the solid never adopts a broken shape.
void PropertiesPanel::applyFeatureEdit(int paramIndex)
{
    if (paramIndex < 0 ||
        paramIndex >= static_cast<int>(m_featureParams.size()))
        return;
    const featureparams::Param p =
        m_featureParams[static_cast<size_t>(paramIndex)];
    const int row = m_featureRowStart + paramIndex;
    const QString text = m_geomTable->item(row, 1)->text().trimmed();
    bool numOk = false;
    const double value = text.toDouble(&numOk);
    // No positivity check here: centre coordinates are legitimately signed.
    // featureparams::set enforces per-parameter validity (lengths > 0).
    if (!numOk) {
        rebuildGeometryTable(); // revert display
        return;
    }
    const EntityId id = m_selection->ids().front();

    m_doc->beginTransaction(QStringLiteral("FEATURE EDIT"));
    bool applied = false;
    QString error;
    if (Entity* e = m_doc->beginModify(id)) {
        auto* solid = dynamic_cast<SolidEntity*>(e);
        if (solid && solid->features &&
            featureparams::set(*solid->features, p.nodeIndex, p.name, value)) {
            if (solid->regenerateFeatures()) {
                applied = true;
            } else {
                // Grab the replay's reason, then put the old value back.
                error = solid->features->regenerate().message;
                featureparams::set(*solid->features, p.nodeIndex, p.name,
                                   p.value);
            }
        }
        m_doc->endModify(id);
    }
    if (applied) {
        m_doc->commitTransaction();
        emit propertiesApplied();
    } else {
        m_doc->rollbackTransaction();
        if (error.isEmpty())
            error = QStringLiteral("feature edit rejected");
        emit feedback(QStringLiteral("%1 = %2: %3").arg(p.label).arg(value).arg(error));
    }
    rebuildGeometryTable();
}

void PropertiesPanel::applyOriginEdit(int axis)
{
    if (axis < 0 || axis > 2 || m_originRowStart < 0)
        return;
    const int row = m_originRowStart + axis;
    const QString text = m_geomTable->item(row, 1)->text().trimmed();
    bool numOk = false;
    const double target = text.toDouble(&numOk);
    if (!numOk) {
        rebuildGeometryTable(); // revert display
        return;
    }
    const EntityId id = m_selection->ids().front();
    const auto* probe = dynamic_cast<const SolidEntity*>(m_doc->entity(id));
    if (!probe)
        return;
    const double current = axis == 0   ? probe->bounds().min.x
                           : axis == 1 ? probe->bounds().min.y
                                       : probe->zMin();
    const double delta = target - current;
    if (std::fabs(delta) < 1e-12) {
        rebuildGeometryTable();
        return;
    }
    // Translate the solid so its bounding-box corner lands on the typed
    // coordinate — a "move by typing", undoable like any transaction.
    gp_Trsf t;
    t.SetTranslation(gp_Vec(axis == 0 ? delta : 0.0, axis == 1 ? delta : 0.0,
                            axis == 2 ? delta : 0.0));
    m_doc->beginTransaction(QStringLiteral("MOVE"));
    if (Entity* e = m_doc->beginModify(id)) {
        if (auto* solid = dynamic_cast<SolidEntity*>(e))
            solid->applyTrsf(t);
        m_doc->endModify(id);
    }
    m_doc->commitTransaction();
    emit propertiesApplied();
    rebuildGeometryTable();
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
