# 0002 — writeEntity: emit entity XDATA

**File:** `src/libdxfrw.cpp`, `dxfRW::writeEntity`
**Date:** 2026-07-07

`writeExtData` existed but was only called for layers; `DRW_Entity::extData`
(shared_ptr list) was parsed on read yet silently dropped on write. Patch
converts the list and calls `writeExtData` at the end of `writeEntity`.
Needed for VIKI_STICKYNOTE XDATA round-trips. Logged in docs/DEVLOG.md.
