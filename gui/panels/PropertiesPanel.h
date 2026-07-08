#pragma once

#include <functional>

#include <QWidget>

#include "cmd/CommandProcessor.h"

class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;

namespace viki {

// Properties of the current selection: layer and color, applied as one
// journaled transaction.
class PropertiesPanel : public QWidget {
    Q_OBJECT
public:
    explicit PropertiesPanel(QWidget* parent = nullptr);

    void attach(Document* doc, SelectionSet* selection);
    void refresh();

signals:
    void propertiesApplied();

private:
    void layerPicked(int index);
    void colorClicked();
    void byLayerClicked();
    void applyToSelection(const std::function<void(Entity&)>& fn, const QString& txName);
    void rebuildGeometryTable();
    void geometryCellChanged(int row, int column);

    Document* m_doc = nullptr;
    SelectionSet* m_selection = nullptr;
    QLabel* m_countLabel = nullptr;
    QComboBox* m_layerCombo = nullptr;
    QPushButton* m_colorBtn = nullptr;
    QPushButton* m_byLayerBtn = nullptr;
    QTableWidget* m_geomTable = nullptr;
    bool m_refreshing = false;
};

} // namespace viki
