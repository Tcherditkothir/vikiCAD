# 0001 — writeSpline: emit fit points

**File:** `src/libdxfrw.cpp`, `dxfRW::writeSpline`
**Date:** 2026-07-07

Upstream never writes the fit-point list (group codes 11/21/31), although the
reader parses it and code 74 (fit count) was already written. A fit-point-only
spline therefore came back empty after an export→import round trip.

Patch adds the fit-point loop after the control-point loop, mirroring the
reader. Logged in docs/DEVLOG.md.
