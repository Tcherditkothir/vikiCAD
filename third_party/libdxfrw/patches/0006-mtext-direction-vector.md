# Patch 0006 — MTEXT rotation as a direction vector, not group 50

**File:** `src/libdxfrw.cpp` (`writeMText`)

**Problem.** `writeMText()` emitted the rotation as group `50`. The DXF spec
allows it (radians — though plenty of producers write degrees, which is why
readers end up guessing), but GNU LibreDWG's DXF reader does not know group
50 on MTEXT and treats it as **fatal**: `ERROR: Invalid DXF code 50 for
MTEXT` → `Failed to decode DXF file` → `dxf2dwg` writes no DWG at all. One
rotated label was enough to sink a whole 5 186-entity export (Pyramide,
2026-07-26).

**Fix.** Write what AutoCAD itself writes: the X-axis direction vector
`11/21/31` (`cos(angle)`, `sin(angle)`, `0`), and no group 50. `DRW_MText::
angle` is radians in this writer (the code-50 convention it always used).
Round-trip safety: libdxfrw's own reader already handles 11
(`hasXAxisVec`), and VikiCAD's importer prefers the vector over the
ambiguous 50 ("the reliable source", DxfImporter::addMText) — so this also
removes a radians-vs-degrees guess from our own reimport path.
