#pragma once

#include <QWidget>

#include <AIS_InteractiveContext.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>

#include "doc/Document.h"

namespace viki {

// OCCT AIS 3D view (shaded solids). Rotate = left drag, pan = middle drag,
// zoom = wheel. Rebuilt from the document on every activation.
class OcctViewWidget : public QWidget {
    Q_OBJECT
public:
    explicit OcctViewWidget(QWidget* parent = nullptr);

    // (Re)display every solid in the document and fit the view.
    void refreshFrom(const Document& doc);
    bool isReady() const { return !m_view.IsNull(); }
    // Dump the 3D framebuffer to an image file (QWidget::grab can't capture
    // OCCT's native GL window). Used by the screenshot IPC in 3D mode.
    bool dumpToFile(const QString& path);

signals:
    // Emitted on pick: a short human-readable description of what's selected.
    void picked(const QString& info);

protected:
    QPaintEngine* paintEngine() const override { return nullptr; }
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void initViewer();

    Handle(V3d_Viewer) m_viewer;
    Handle(V3d_View) m_view;
    Handle(AIS_InteractiveContext) m_context;
    QPoint m_lastPos;
    QPoint m_pressPos;
    bool m_initFailed = false;
};

} // namespace viki
