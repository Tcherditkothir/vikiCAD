# Changelog

## 0.2.0 — 2026-07-17

Fusion-style 3D interaction, full headless/agent parity, and a new PCB
fabrication (Gerber/Excellon) editor.

- **3D interaction**: the 3D view is an input device — hovering a face sets
  the work plane and drives the ghost preview (red = material removed, blue
  = added); box-select drag, right-drag orbit / short right-click = a
  tree-structured context menu (Hole ▸/Face ▸/Edges ▸/Move ▸/Select ▸);
  Alt+click / "Select ▸" opens a candidate resolver that highlights each
  option, including an X-ray ghost that glows *through* occluding solids;
  ViewCube; SPLIT/COMBINE solids by a plane or curved face; a parametric
  `FeatureTree` (hole/shell/extrude…) editable from the Properties panel.
- **Sketches v1**: lightweight, named, drawn on any face or work plane,
  visible in the 3D view, with no dependency from an already-generated
  solid back onto its source sketch (editing a used-up sketch never
  regenerates the part).
- **Multi-STEP assemblies**: `ASSEMBLY` command, assembly tree panel,
  per-solid color/transparency, and multi-file selection when inserting STEP
  components.
- **Agent parity**: every mouse action has a headless equivalent —
  index-addressed `INSPECT`/`FEATEDIT`/`PUSHPULL`/`SHELLOPEN`/`SPLITFACE`/
  `FILLETEDGES`/`CHAMFEREDGES`/`MATE`/`DRAFT`, `DESCRIBE` + `query --describe`
  (computed volume/area/bbox/centroid/features, no BREP blob), IPC `viewdir`
  for a view→screenshot→diff loop. The complete guide,
  **[docs/AGENT.md](docs/AGENT.md)**, was validated end-to-end by an agent
  given nothing but the guide itself.
- **PCB fabrication editor (new)**: read, inspect, measure, edit and
  re-export **Gerber RS-274X and Excellon** files — without a full EDA suite.
  Open a whole fab-output kit (directory or single file, GUI/CLI/IPC) with
  one layer per file, correct negative (LPC) polarity rendering, a CAM-style
  layer stack (per-layer transparency/paint order/role, mirrored bottom
  view), edge-to-edge clearance measurement (`MINDIST`), an aperture
  inspector, drill reports, and an RS-274X/Excellon writer whose output was
  checked pixel-for-pixel against `gerbv` (the reference renderer) on real
  fabrication kits. `PANELIZE`, and a DXF↔Gerber bridge (draw a board
  outline in 2D, export it as a clean `.GKO`). See
  **[docs/PCB_CAM.md](docs/PCB_CAM.md)**.
- File > Export (STEP/DXF/STL/OBJ/Gerber kit) and the matching IPC `export`
  verb.
- Hardening: previously-dead Ctrl+Z/Ctrl+Y (ambiguous Qt shortcuts), booleans
  that silently produced zero solids, leaked-transaction undo corruption —
  all fixed at the root with regression tests.
- Test suite: 1506 → **5142 assertions across 334 cases**; the live-GUI
  regression harness (`scripts/gui-smoke.sh`): 0 → **224 checks**, including
  an automated visual diff of Gerber output against `gerbv`.

## 0.1.0 — 2026-07-09

First public release.

- **2D drafting**: LINE/CIRCLE/ARC/RECT/PLINE/ELLIPSE/SPLINE/POINT/XLINE;
  MOVE/COPY/ROTATE/MIRROR/SCALE/STRETCH; TRIM/EXTEND/OFFSET/FILLET/CHAMFER/
  BREAK/JOIN/EXPLODE; object snaps, ORTHO/POLAR/GRID, vertex grips, layers,
  mm/inch toggle.
- **Annotation**: MTEXT, five dimension types regenerated live from DimStyle,
  leaders, hatches, MATCHPROP.
- **Organization**: blocks with attributes, associative rect/polar arrays,
  sticky notes (markdown, pinnable to entities), layouts with exact-scale PDF
  plotting.
- **Interop**: DXF R12–2018 import/export (vendored libdxfrw), DWG import via
  external `dwg2dxf`, STEP round-trip with sidecar notes.
- **3D solids**: work planes, EXTRUDE/REVOLVE from closed 2D profiles,
  UNION/SUBTRACT/INTERSECT, FILLET3D/CHAMFER3D, push/pull on faces, shaded
  OCCT view with interactive 3D selection, parametric feature tree.
- **Assembly**: multi-STEP component tree with per-solid color/transparency.
- **Export**: STL/OBJ mesh export, PDF plots, DXF, STEP.
- **Automation**: headless `vikicad-cli` with single-line JSON output, `.vks`
  command scripts (AutoCAD `.scr` semantics), JSON-RPC IPC socket on the
  running GUI (`vikicad-cli connect`).
- Native SQLite `.vkd` file format with full undo journal.
