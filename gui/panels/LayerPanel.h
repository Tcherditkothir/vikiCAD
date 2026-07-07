#pragma once

#include <QWidget>

#include "doc/Document.h"

class QTableWidget;

namespace viki {

// Layer manager: current layer, name, color, visible, locked. Layer table
// operations are direct (not journaled) by design for v1.
class LayerPanel : public QWidget {
    Q_OBJECT
public:
    explicit LayerPanel(QWidget* parent = nullptr);

    void attach(Document* doc);
    void refresh();

signals:
    void layersChanged();

private:
    void addLayerClicked();
    void removeLayerClicked();
    void cellChanged(int row, int column);
    void cellClicked(int row, int column);

    Document* m_doc = nullptr;
    QTableWidget* m_table = nullptr;
    bool m_refreshing = false;
};

} // namespace viki
