#pragma once

#include <functional>

#include <QString>

class QHBoxLayout;
class QMainWindow;
class QTabWidget;
class QToolButton;
class QWidget;

namespace viki {

// Builds the menu-bar menus mirroring the tool groups (Draw / Modify /
// Annotate / Measure / Blocks / Solids). Each entry submits its command line
// through `runCommand` — exactly the same path as typing it.
void buildToolMenus(QMainWindow* window,
                    const std::function<void(const QString&)>& runCommand);

// Builds the compact tool tab strip: one tab per group, each page a single
// horizontal row of tool buttons (same commands as the menus, same tables).
// The caller owns placement (typically inside a QToolBar above the view).
QTabWidget* buildToolTabs(QWidget* parent,
                          const std::function<void(const QString&)>& runCommand);

// Appends an extra (custom) page to a tab strip built by buildToolTabs and
// returns its row layout, so callers can add non-command buttons — used by
// MainWindow for the Views tab (Top/Front/... act on the 3D view directly).
QHBoxLayout* addToolTabPage(QTabWidget* tabs, const QString& title);

// One tool button in the house style (glyph over label, tooltip), appended
// to a row created by addToolTabPage.
QToolButton* addToolButton(QHBoxLayout* row, const QString& glyph,
                           const QString& label, const QString& tip);

} // namespace viki
