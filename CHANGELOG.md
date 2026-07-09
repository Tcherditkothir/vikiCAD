# Changelog

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
