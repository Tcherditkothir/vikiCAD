#include "ToolPanels.h"

#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QToolBar>
#include <QToolButton>

namespace viki {
namespace {

struct Tool {
    const char* glyph;
    const char* label;
    const char* command;
    const char* tip;
};

struct Panel {
    const char* title;
    std::initializer_list<Tool> tools;
};

const Panel kPanels[] = {
    {"Draw",
     {{"╱", "Line", "LINE", "Line — click points, Enter to finish (L)"},
      {"◯", "Circle", "CIRCLE", "Circle — center + radius (C)"},
      {"◠", "Arc", "ARC", "Arc through 3 points (A)"},
      {"▭", "Rect", "RECT", "Rectangle — 2 corners"},
      {"⺄", "Pline", "PLINE", "Polyline — points, C to close (PL)"},
      {"⬭", "Ellipse", "ELLIPSE", "Ellipse — center + axes (EL)"},
      {"∿", "Spline", "SPLINE", "Spline through fit points (SPL)"},
      {"·", "Point", "POINT", "Point (PO)"},
      {"⤫", "XLine", "XLINE", "Construction line (XL)"},
      {"▦", "Hatch", "HATCH", "Select closed boundaries FIRST, then click; Enter twice keeps pattern/scale defaults (H)"}}},
    {"Modify",
     {{"⇄", "Move", "MOVE", "Move (M)"},
      {"⧉", "Copy", "COPY", "Copy (CO)"},
      {"⟳", "Rotate", "ROTATE", "Rotate (RO)"},
      {"⇋", "Mirror", "MIRROR", "Mirror (MI)"},
      {"⤢", "Scale", "SCALE", "Scale (SC)"},
      {"✂", "Trim", "TRIM", "Trim to cutting edges (TR)"},
      {"⇥", "Extend", "EXTEND", "Extend to boundary (EX)"},
      {"∥", "Offset", "OFFSET", "Parallel offset (O)"},
      {"◟", "Fillet", "FILLET", "Fillet two lines, R for radius (F)"},
      {"◺", "Chamfer", "CHAMFER", "Chamfer two lines, D for distances (CHA)"},
      {"⊟", "Break", "BREAK", "Break an entity (BR)"},
      {"⋈", "Join", "JOIN", "Join lines/arcs (J)"},
      {"💥", "Explode", "EXPLODE", "Explode polylines/blocks/arrays (X)"},
      {"⇱", "Stretch", "STRETCH", "Stretch with a crossing window (S)"},
      {"⌫", "Erase", "ERASE", "Erase (E)"},
      {"🖌", "Match", "MATCHPROP", "Copy layer/color to other entities (MA)"}}},
    {"Annotate",
     {{"T", "Text", "TEXT", "Multiline text (T)"},
      {"⟷", "Linear", "DIMLINEAR", "Linear dimension (DLI)"},
      {"⤡", "Aligned", "DIMALIGNED", "Aligned dimension (DAL)"},
      {"∠", "Angle", "DIMANGULAR", "Angular dimension (DAN)"},
      {"R", "Radius", "DIMRADIUS", "Radius dimension (DRA)"},
      {"Ø", "Diam", "DIMDIAMETER", "Diameter dimension (DDI)"},
      {"↖", "Leader", "LEADER", "Leader with text (LE)"},
      {"📌", "Note", "NOTE", "Sticky note at a point"},
      {"📍", "Pin", "NOTEPIN", "Sticky note pinned to an entity"},
      {"⚙", "DimSty", "DIMSTYLE", "Dimension style settings (DST)"}}},
    {"Measure",
     {{"📏", "Dist", "DIST", "Distance, deltas and angle between 2 points (DI)"},
      {"⬛", "Area", "AREA", "Area/perimeter of clicked corners, or E to pick an entity (AA)"},
      {"⌖", "ID", "ID", "Coordinates of a point"},
      {"ℹ", "List", "LIST", "Full info about a picked entity (LI)"}}},
    {"Blocks & 3D",
     {{"▣", "Block", "BLOCK", "Create a block from entities (B)"},
      {"⊕", "Insert", "INSERT", "Insert a block (I)"},
      {"⠿", "ArrRect", "ARRAYRECT", "Rectangular associative array (AR)"},
      {"❋", "ArrPol", "ARRAYPOLAR", "Polar associative array (AP)"},
      {"⬆", "Extrude", "EXTRUDE", "Extrude a closed profile (EXT)"},
      {"◎", "Revolve", "REVOLVE", "Revolve a profile around an axis (REV)"},
      {"∪", "Union", "UNION", "Fuse two solids"},
      {"∖", "Subtr", "SUBTRACT", "Subtract solid B from A (SUB)"},
      {"∩", "Inters", "INTERSECT", "Intersection of two solids (INT)"},
      {"◜", "Fillet3D", "FILLET3D", "Round all edges of a solid (F3D)"}}},
};

} // namespace

void buildToolPanels(QMainWindow* window, QMenu* viewMenu,
                     const std::function<void(const QString&)>& runCommand)
{
    // Full menus mirroring the toolbars: every command reachable from the
    // menu bar, hover shows "COMMAND — description" (tooltips enabled).
    QMenuBar* menuBar = window->menuBar();
    for (const Panel& panel : kPanels) {
        auto* menu = menuBar->addMenu(QLatin1String(panel.title));
        menu->setToolTipsVisible(true);
        for (const Tool& tool : panel.tools) {
            auto* action = menu->addAction(
                QStringLiteral("%1  %2").arg(QString::fromUtf8(tool.glyph),
                                             QLatin1String(tool.label)));
            const QString tip = QStringLiteral("%1  —  %2")
                                    .arg(QLatin1String(tool.command),
                                         QString::fromUtf8(tool.tip));
            action->setToolTip(tip);
            action->setStatusTip(tip);
            const QString command = QLatin1String(tool.command);
            QObject::connect(action, &QAction::triggered, window,
                             [runCommand, command] { runCommand(command); });
        }
    }

    for (const Panel& panel : kPanels) {
        auto* bar = new QToolBar(QLatin1String(panel.title), window);
        bar->setObjectName(QLatin1String(panel.title));
        bar->setMovable(true);
        bar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        for (const Tool& tool : panel.tools) {
            auto* btn = new QToolButton(bar);
            btn->setText(QString::fromUtf8(tool.glyph) + QLatin1Char('\n') +
                         QLatin1String(tool.label));
            btn->setToolTip(QStringLiteral("%1  —  %2")
                                .arg(QLatin1String(tool.command),
                                     QString::fromUtf8(tool.tip)));
            btn->setAutoRaise(true);
            const QString command = QLatin1String(tool.command);
            QObject::connect(btn, &QToolButton::clicked, window,
                             [runCommand, command] { runCommand(command); });
            bar->addWidget(btn);
        }
        window->addToolBar(Qt::TopToolBarArea, bar);
        if (viewMenu)
            viewMenu->addAction(bar->toggleViewAction());
    }
}

} // namespace viki
