#pragma once

#include <QWidget>

#include "doc/Document.h"

class QTableWidget;

namespace viki {

// Layer manager: current layer, name, color, visible, locked, alpha
// (compositing opacity %), paint rank (lower paints first; ▲/▼ move the
// selected layer in the stack) and the CAM Gerber role (context menu
// "Set Gerber role" — reassigning recolors to the role palette). Layer
// table operations are direct (not journaled) by design for v1.
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
    void moveLayerClicked(int delta); // +1 = painted later (on top)
    void cellChanged(int row, int column);
    void cellClicked(int row, int column);
    void showContextMenu(const QPoint& pos);
    LayerId layerIdAtRow(int row) const;

    Document* m_doc = nullptr;
    QTableWidget* m_table = nullptr;
    bool m_refreshing = false;
};

} // namespace viki
