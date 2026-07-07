#include "Theme.h"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

namespace viki {

void applyDarkTheme(QApplication& app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    // Palette tuned to sit well with the canvas background (24,26,28).
    const QColor window(32, 34, 37);
    const QColor base(24, 26, 28);
    const QColor alt(40, 43, 47);
    const QColor text(222, 224, 227);
    const QColor dim(140, 145, 150);
    const QColor accent(64, 132, 214);

    QPalette p;
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, alt);
    p.setColor(QPalette::ToolTipBase, alt);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::PlaceholderText, dim);
    p.setColor(QPalette::Button, window);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, accent.lighter(120));
    p.setColor(QPalette::Disabled, QPalette::Text, dim.darker(120));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, dim.darker(120));
    p.setColor(QPalette::Disabled, QPalette::WindowText, dim.darker(120));
    app.setPalette(p);

    app.setStyleSheet(QStringLiteral(R"css(
QMainWindow::separator { background: #17181a; width: 3px; height: 3px; }

QDockWidget { titlebar-close-icon: none; titlebar-normal-icon: none; }
QDockWidget::title {
    background: #26282c; padding: 5px 10px;
    border-bottom: 1px solid #17181a;
    font-weight: 600; letter-spacing: 0.5px;
}

QMenuBar { background: #202225; border-bottom: 1px solid #17181a; padding: 2px; }
QMenuBar::item { padding: 5px 10px; border-radius: 4px; }
QMenuBar::item:selected { background: #33363b; }
QMenu { background: #26282c; border: 1px solid #17181a; padding: 4px; }
QMenu::item { padding: 5px 26px 5px 14px; border-radius: 4px; }
QMenu::item:selected { background: #4084d6; }

QStatusBar { background: #202225; border-top: 1px solid #17181a; }
QStatusBar QToolButton {
    background: #2b2e33; border: 1px solid #17181a; border-radius: 4px;
    padding: 3px 10px; margin: 2px 1px; font-weight: 600; color: #9aa0a6;
}
QStatusBar QToolButton:hover { background: #34383e; color: #dee0e3; }
QStatusBar QToolButton:checked {
    background: #2d4f79; border-color: #4084d6; color: #cfe3fb;
}

QLineEdit, QPlainTextEdit, QComboBox {
    background: #1a1c1e; border: 1px solid #33363b; border-radius: 4px;
    padding: 4px 6px; selection-background-color: #4084d6;
}
QLineEdit:focus, QComboBox:focus { border-color: #4084d6; }

QPushButton {
    background: #2b2e33; border: 1px solid #3a3e44; border-radius: 4px;
    padding: 5px 14px;
}
QPushButton:hover { background: #34383e; border-color: #4084d6; }
QPushButton:pressed { background: #2d4f79; }

QTableWidget {
    background: #1a1c1e; gridline-color: #26282c;
    border: none; alternate-background-color: #202225;
}
QHeaderView::section {
    background: #26282c; border: none; border-right: 1px solid #17181a;
    border-bottom: 1px solid #17181a; padding: 4px 6px; font-weight: 600;
}

QScrollBar:vertical { background: #202225; width: 11px; margin: 0; }
QScrollBar::handle:vertical {
    background: #3a3e44; border-radius: 5px; min-height: 30px; margin: 2px;
}
QScrollBar::handle:vertical:hover { background: #4a4f56; }
QScrollBar:horizontal { background: #202225; height: 11px; margin: 0; }
QScrollBar::handle:horizontal {
    background: #3a3e44; border-radius: 5px; min-width: 30px; margin: 2px;
}
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }

QToolTip {
    background: #26282c; color: #dee0e3; border: 1px solid #4084d6;
    padding: 4px 6px;
}
)css"));
}

} // namespace viki
