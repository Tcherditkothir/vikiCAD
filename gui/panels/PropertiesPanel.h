#pragma once

#include <functional>
#include <vector>

#include <QWidget>

#include "cmd/CommandProcessor.h"
#include "solid/FeatureParams.h"

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
    // One-line user feedback (e.g. why a feature edit was refused); routed to
    // the command bar history by MainWindow.
    void feedback(const QString& message);

private:
    void layerPicked(int index);
    void colorClicked();
    void byLayerClicked();
    void applyToSelection(const std::function<void(Entity&)>& fn, const QString& txName);
    void rebuildGeometryTable();
    void geometryCellChanged(int row, int column);
    void applyFeatureEdit(int paramIndex);
    void applyOriginEdit(int axis); // 0=x 1=y 2=z — translates the solid

    Document* m_doc = nullptr;
    SelectionSet* m_selection = nullptr;
    QLabel* m_countLabel = nullptr;
    QComboBox* m_layerCombo = nullptr;
    QPushButton* m_colorBtn = nullptr;
    QPushButton* m_byLayerBtn = nullptr;
    QTableWidget* m_geomTable = nullptr;
    bool m_refreshing = false;
    // Feature-parameter rows appended after the geometry rows when the single
    // selected entity is a solid with a parametric history. m_featureRowStart
    // is the table row of m_featureParams[0] (-1 = no feature rows).
    std::vector<featureparams::Param> m_featureParams;
    int m_featureRowStart = -1;
    // "origin x/y/z" rows for a solid (its bounding-box corner). Editing one
    // TRANSLATES the solid so the corner lands on the typed coordinate.
    int m_originRowStart = -1;
};

} // namespace viki
