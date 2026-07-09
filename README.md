# VikiCAD

2D drafting + 3D solid CAD for Linux. Personal replacement for nanoCAD.

- **Stack:** C++17, Qt6 Widgets, OpenCASCADE 7.9, SQLite native format (`.vkd`), CMake.
- **Interop:** DXF R12–2018 (vendored libdxfrw, patched) and STEP (OCCT) with
  sticky notes carried as XDATA (DXF), sidecar JSON + optional AP242
  user-defined attributes (STEP, `VIKICAD_STEP_UDA=1`).
- **Agent-friendly by design:** headless `vikicad-cli` with JSON output,
  JSON-RPC socket on the running GUI (`vikicad-cli connect ...`), `.vks`
  command scripts with AutoCAD .scr semantics.

## Features

**2D drafting** — LINE/CIRCLE/ARC/RECT/PLINE/ELLIPSE/SPLINE/POINT/XLINE;
MOVE/COPY/ROTATE/MIRROR/SCALE/STRETCH; TRIM/EXTEND/OFFSET/FILLET/CHAMFER/
BREAK/JOIN/EXPLODE; object snaps (endpoint/midpoint/center/quadrant/
intersection/perpendicular) + ORTHO/POLAR/GRID; vertex grips; layers;
mm/inch one-click toggle (`x,y`, `@dx,dy`, `@d<angle`, `2"`, `10mm`).

**Annotation** — MTEXT, 5 dimension kinds regenerated live from DimStyle and
display units, leaders, hatches (SOLID/ANSI31/ANSI37), MATCHPROP.

**Organization** — blocks with attributes, associative rect/polar arrays
(live-editable via ARRAYEDIT), **sticky notes** (markdown, author,
timestamps, pinnable to entities — VikiCAD's signature feature), layouts +
exact-scale PDF plotting.

**3D** — work planes (XY/offset), EXTRUDE/REVOLVE from any closed 2D profile
(including chained lines/arcs), UNION/SUBTRACT/INTERSECT, FILLET3D/CHAMFER3D,
shaded OCCT view (3D toggle), STEP round-trip.

## Build

```sh
sudo apt install build-essential cmake ninja-build qt6-base-dev \
    libocct-foundation-dev libocct-modeling-algorithms-dev \
    libocct-modeling-data-dev libocct-data-exchange-dev \
    libocct-visualization-dev libocct-ocaf-dev \
    catch2 libzstd-dev libsqlite3-dev
scripts/build-and-test.sh
```

## CLI for agents

```sh
vikicad-cli new --run drawing.vks --save-as part.vkd
vikicad-cli open part.vkd --exec "CIRCLE 50,50 10" --exec "EXTRUDE 20 1" --save
vikicad-cli query part.vkd --entities --layers --notes --blocks --layouts
vikicad-cli export part.vkd out.dxf --dxf-version 2013
vikicad-cli export part.vkd out.pdf --layout SHEET
vikicad-cli export part.vkd out.step
vikicad-cli import legacy.dxf --save-as legacy.vkd
vikicad-cli connect exec "LINE 0,0 100,0"   # drive the running GUI
```

All output is single-line JSON: `{"ok":true,"result":{...}}`.

## License

VikiCAD is free software, released under the **GNU General Public License,
version 3 or (at your option) any later version** (GPLv3-or-later). See
[LICENSE](LICENSE) for the full text.

The combined work is distributed under GPLv3 because the vendored DXF library
(libdxfrw) is GPLv2-or-later. Dependency licenses:

| Dependency | License | Linkage |
|---|---|---|
| libdxfrw (vendored, patched) | GPL-2.0-or-later | statically linked |
| OpenCASCADE (OCCT) | LGPL-2.1 with OCCT exception | dynamically linked |
| Qt 6 | LGPL-3.0 | dynamically linked |
| SQLite | Public domain | dynamically linked |
| Catch2 | BSL-1.0 | tests only |
| LibreDWG (`dwg2dxf`) | GPL-3.0 | external binary invoked at runtime, not linked |

## Docs

- `docs/DEVLOG.md` — development journal (one entry per milestone).
- `docs/LESSONS.md` — running log of mistakes and lessons learned.
- `third_party/libdxfrw/patches/` — vendored-library patches.
