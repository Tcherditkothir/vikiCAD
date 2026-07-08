# Patch 0004 — tolerate (and rejoin) raw newlines in ASCII DXF values

**File:** `src/intern/dxfreader.cpp`, `src/intern/dxfreader.h`

**Problem.** LibreDWG's `dwg2dxf` (our AC1032/2018+ DWG fallback) wraps long
string values at a fixed **254 bytes** by inserting a raw `CR/LF` *in the
middle of the value*. The spill-over physical line then carries **no group
code**. Stock libdxfrw feeds that line to `atoi()` where a group code is
expected, gets `0` ("end of entity"), and every following group is parsed
shifted by one line — silently dropping thousands of entities (the original
"tomato bug": `Domaine_Bichonnerie` imported 0 / 12 522 model entities).

**First fix (shipped).** `readCode()` skipped non-numeric lines as value
continuations. That stopped the parse shift but **discarded** the spilled
text, truncating MTEXT — e.g. `Immeuble protégé … = 1,0 / Maison … / Périmètre
…` collapsed to `Immeuble protég` (accented char + whole tail lost).

**Current fix.** A one-line lookahead (`nextRawLine`/`pushRawLine`) lets the
string readers **rejoin** continuations byte-for-byte (the injected newline is
dropped, reconstructing the pre-wrap value, robust even to a UTF-8 char split
across the boundary). It is gated on the wrap width so well-formed DXF is
never touched:

- a value is only extended when its segment length ≥ `kDwgWrapWidth` (254) and
  the next line is **not** a group code (`looksLikeGroupCode`);
- in valid DXF a value is always followed by a numeric code line, so the join
  never triggers there — verified: all sample files import with identical
  entity counts, full test suite green.

`readCode()` keeps the non-numeric skip as a bounded backstop for spills that
land after a value shorter than the wrap width.

**Tests.** `tests/test_dxf_import.cpp`:
- *DXF reader rejoins dwg2dxf raw-newline value spills* — 254-byte segment +
  raw-newline continuation ending in `protégé tail`, asserted rejoined.
- *DXF reader leaves normal short values untouched* — a short value must not
  absorb the following code line.
