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

// Menu coverage contract: every command registered on the CommandProcessor
// (grep `registerCommand` + `name() const override` in core/cmd) must be
// reachable from a menu-bar menu with an English label — either from a panel
// below or from MainWindow.cpp's File/Edit/View menus. Checked by
// scripts/check-menu-coverage.sh (run by hand or in CI; it greps both files).
// Intentionally NOT in the panels below (they live in MainWindow.cpp menus):
//   UNDO/REDO  — Edit menu (single home for the Ctrl+Z/Ctrl+Y bindings)
//   ZOOM       — View menu ("Zoom...")
//   LAYOUT/PLOT — File menu ("Page Layout...", "Plot / Print...")
// ERASE appears both in Modify and as Edit > Delete selection.
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
      {"▦", "Hatch", "HATCH", "Select closed boundaries FIRST, then click; Enter twice keeps pattern/scale defaults (H)"},
      {"⚙", "Gear", "GEAR", "Involute spur gear from module/teeth/pressure-angle, with a design note (SPURGEAR)"}}},
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
      {"🗒", "NoteEdit", "NOTEEDIT", "Edit the text of an existing sticky note"},
      {"✎", "TextEdit", "TEXTEDIT", "Edit a text entity — double-clicking text also opens it (ED)"},
      {"⚒", "DimSty", "DIMSTYLE", "Dimension style settings (DST)"}}},
    {"Measure",
     {{"📏", "Dist", "DIST", "Distance, deltas and angle between 2 points (DI)"},
      {"⬛", "Area", "AREA", "Area/perimeter of clicked corners, or E to pick an entity (AA)"},
      {"⌖", "ID", "ID", "Coordinates of a point"},
      {"ℹ", "List", "LIST", "Full info about a picked entity (LI)"}}},
    {"Blocks",
     {{"▣", "Block", "BLOCK", "Create a block from entities (B)"},
      {"⊕", "Insert", "INSERT", "Insert a block (I)"},
      {"🏷", "AttDef", "ATTDEF", "Attribute definition inside a block (ATT)"},
      {"⠿", "ArrRect", "ARRAYRECT", "Rectangular associative array (AR)"},
      {"❋", "ArrPol", "ARRAYPOLAR", "Polar associative array (AP)"},
      {"✏", "ArrEdit", "ARRAYEDIT", "Edit the counts/spacing of an associative array"}}},
    // The complete 3D set: creation, dress-up, booleans, placement, patterns,
    // inspection and drawing output — one coherent home (was "Blocks & 3D").
    {"Solids",
     {{"⌗", "WPlane", "WORKPLANE", "Set the sketch work plane (WP)"},
      {"⬆", "Extrude", "EXTRUDE", "Extrude a closed profile — Blind/Symmetric/ToFace modes (EXT)"},
      {"◎", "Revolve", "REVOLVE", "Revolve a profile around an axis (REV)"},
      {"⤳", "Sweep", "SWEEP", "Sweep a profile along a path (SW)"},
      {"≋", "Loft", "LOFT", "Loft through profile sections (LO)"},
      {"◉", "Hole", "HOLE", "Drilled hole into a solid face (HO)"},
      {"回", "Shell", "SHELL", "Hollow a solid leaving a wall thickness (SH)"},
      {"◜", "Fillet3D", "FILLET3D", "Round all edges of a solid (F3D)"},
      {"◿", "Chamf3D", "CHAMFER3D", "Chamfer all edges of a solid (CH3D)"},
      {"∪", "Union", "UNION", "Fuse two solids"},
      {"∖", "Subtr", "SUBTRACT", "Subtract solid B from A (SUB)"},
      {"∩", "Inters", "INTERSECT", "Intersection of two solids (INT)"},
      {"⊞", "Combine", "COMBINE", "Fuse many solids into one (FUSE)"},
      {"⊟", "Split", "SPLIT", "Split a solid with a plane (SPLITBODY)"},
      {"⇄", "Move3D", "MOVE3D", "Move a solid by a 3D delta (M3)"},
      {"⟳", "Rot3D", "ROTATE3D", "Rotate a solid around an axis (RO3)"},
      {"⿲", "Pat3D", "PATTERN3D", "Rectangular 3D pattern of a solid (P3R)"},
      {"❂", "PatPol3D", "PATTERNPOLAR3D", "Polar 3D pattern around an axis (P3P)"},
      {"◍", "Opacity", "TRANSPARENCY", "Set a solid's transparency (TRANS)"},
      {"📐", "Meas3D", "MEASURE3D", "Volume/area/bbox of a solid (M3D)"},
      {"⚡", "Clash", "INTERFERE", "Interference check between solids (CLASH)"},
      {"⧄", "Section", "SECTION", "Section a solid with a plane (SEC)"},
      {"🗺", "MakeView", "MAKEVIEW", "HLR drawing view of solids onto the sheet (MV)"},
      {"ƒ", "Param", "PARAM", "Define a named parameter for feature expressions"}}},
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
