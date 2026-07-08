# 0004 — dxfReaderAscii::readCode: tolerate raw newlines inside string values

**File:** `src/intern/dxfreader.cpp`
**Date:** 2026-07-08

LibreDWG's dwg2dxf writes MTEXT chunks containing literal newlines without
escaping them. The spill-over line lands where a group code is expected;
`atoi` silently yields code 0 ("end of entity") and the entire remainder of
the file is parsed shifted — no error raised, entities silently vanish
(Domaine_Bichonnerie: 0 of 12,500 model entities survived). A group-code
line must be numeric; non-numeric lines are now skipped as value
continuations (bounded by a 64-line guard). Logged in docs/DEVLOG.md.
