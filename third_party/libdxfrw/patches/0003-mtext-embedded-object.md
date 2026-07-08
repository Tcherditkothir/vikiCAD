# 0003 — DRW_MText: ignore the AC2018 "Embedded Object" section (code 101)

**Files:** `src/drw_entities.h/.cpp`, `DRW_MText::parseCode`
**Date:** 2026-07-08

Modern MTEXT (AutoCAD 2018+, also emitted by LibreDWG's dwg2dxf) appends an
`101 Embedded Object` section whose inner group codes reuse 10/11/40/41/…
with different semantics (column geometry). libdxfrw kept feeding them to
the normal parser, overwriting the real height (40), width (41), insertion
point and rotation — texts came out at absurd sizes. All codes after 101
are now ignored for the entity. Logged in docs/DEVLOG.md.
