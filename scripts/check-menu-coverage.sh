#!/usr/bin/env bash
# check-menu-coverage.sh — menu completeness audit.
#
# Asserts that every command registered on the CommandProcessor (i.e. every
# `name() const override` in core/cmd/*.cpp, the source of truth behind
# CommandProcessor::commandNames) appears as a quoted string in the menu code
# (gui/panels/ToolPanels.cpp or gui/MainWindow.cpp), so each one is reachable
# from the menu bar with an English label.
#
# Where the non-panel commands live (all still menu-reachable):
#   UNDO / REDO   — Edit menu (Ctrl+Z / Ctrl+Y bindings live there)
#   ERASE         — Modify panel AND Edit > Delete selection
#   ZOOM          — View > Zoom...
#   LAYOUT / PLOT — File > Page Layout... / Plot / Print...
# Intentionally excluded from menus: none. Command ALIASES (L, C, SPURGEAR,
# CLASH, FUSE...) are not menu items by design — only canonical names are.
#
# Exit 0 = full coverage; exit 1 lists the uncovered commands.

set -u
cd "$(dirname "$0")/.."

CMD_DIR=core/cmd
MENU_FILES=(gui/panels/ToolPanels.cpp gui/MainWindow.cpp)

# Canonical command names: the string returned by name(). Three shapes exist:
#   ... name() const override { return "LINE"; }                  (one line)
#   ... name() const override { return m_copy ? "COPY" : "MOVE"; }  (ternary)
#   ... name() const override { switch... case: return "UNION";   (multi-line,
#       the boolean UNION/SUBTRACT/INTERSECT command)
# so harvest every ALL-CAPS string on a `return` line within a few lines of
# a name() override.
names=$(grep -h -A8 'name() const override' "$CMD_DIR"/*.cpp \
        | grep 'return ' \
        | grep -o '"[A-Z][A-Z0-9]*"' \
        | tr -d '"' | sort -u)

count=$(echo "$names" | wc -l)
if [ "$count" -lt 70 ]; then
    echo "FAIL: only $count command names harvested from $CMD_DIR — extraction broke?" >&2
    exit 1
fi

missing=0
for name in $names; do
    if ! grep -q "\"$name\"" "${MENU_FILES[@]}"; then
        echo "MISSING from menus: $name" >&2
        missing=1
    fi
done

if [ "$missing" -ne 0 ]; then
    echo "FAIL: commands above are registered but unreachable from any menu." >&2
    exit 1
fi
echo "OK: all $count registered commands are reachable from the menu bar."
