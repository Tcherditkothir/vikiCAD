# Patch 0005 — write entity owner handles (330), drop the empty CLASSES section

**Files:** `src/libdxfrw.cpp`, `src/libdxfrw.h`

**Problem.** Stock libdxfrw writes DXF that AutoCAD happens to tolerate but
GNU LibreDWG's `dxf2dwg` (our DWG export back end) cannot digest:

1. For AC1012+ it emits an **empty** `CLASSES` section (`SECTION`/`CLASSES`/
   `ENDSEC` with zero `0 CLASS` entries). LibreDWG's DXF reader aborts on
   "2 CLASSES must be followed by 0 CLASS" — `READ ERROR 0x800`, no DWG at
   all.
2. `writeEntity()` (the common part of every entity) never writes the
   **owner handle** (group 330), although canonical AutoCAD DXF has it on
   every entity and libdxfrw itself writes it for BLOCK/ENDBLK. LibreDWG
   imports the entities but stores them as **orphans of the object map** —
   never linked into `*Model_Space` — so the written DWG reads back as an
   EMPTY drawing (dwg2dxf, AutoCAD alike). Proven headless 2026-07-26:
   3-entity file, round-trip came back with 0 entities; with owners injected
   by hand, all 3 survived.

**Fix.**

- `write()` no longer emits the empty CLASSES section (it is optional, and
  nothing was ever put in it).
- New member `currOwner` tracks who owns the entities being written:
  `"1F"` (the hardcoded `*Model_Space` block record) while in the ENTITIES
  section, the current block record's handle while `writeBlock()` writes a
  block's contents. `writeEntity()` emits `330 currOwner` for AC1015+
  (same version gate as the existing BLOCK/ENDBLK owners; R12 output is
  untouched).

**Not covered.** `writePolyline()` VERTEX children would inherit the model
space owner instead of the POLYLINE's — canonical owners for VERTEX are not
worth the plumbing while VikiCAD only ever writes LWPOLYLINE. Revisit if a
POLYLINE writer path appears.
