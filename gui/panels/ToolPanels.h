#pragma once

#include <functional>

#include <QString>

class QMainWindow;
class QMenu;

namespace viki {

// Builds the grouped command toolbars (Draw / Modify / Annotate / Measure /
// Blocks / Solids). Each button submits its command line through `runCommand` —
// exactly the same path as typing it. Toolbar visibility actions are added
// to `viewMenu`.
void buildToolPanels(QMainWindow* window, QMenu* viewMenu,
                     const std::function<void(const QString&)>& runCommand);

} // namespace viki
