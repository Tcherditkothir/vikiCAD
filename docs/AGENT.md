# AGENT.md — Driving VikiCAD headlessly

The complete guide for an AI agent (or any script) that drives VikiCAD
without a mouse. Every example below was executed verbatim against the
build in `build/debug` before being written down; outputs shown are real.

Conventions used throughout:

```sh
cd /path/to/vikicad                       # the repo root
CLI=$PWD/build/debug/cli/vikicad-cli
cd "$(mktemp -d)"                         # keep artifacts out of the repo
```

All CLI output is exactly one JSON object per invocation on stdout:
`{"ok":true,"result":{...}}` or `{"ok":false,"error":{"code","message"}}`.
Exit code is 0 on ok, 1 on error. Geometry is **millimetres everywhere**.

---

## 1. The two channels

### 1a. Offline CLI — batch work on `.vkd` files, no GUI needed

| Verb | One-line example |
|---|---|
| `new` | `$CLI new --exec "RECT 0,0 40,30" --exec "EXTRUDE 10 1" --save-as part.vkd` |
| `open` | `$CLI open part.vkd --exec "INSPECT 2 All"` — add `--save` / `--save-as out.vkd` to persist |
| `query` | `$CLI query part.vkd --entities --bounds` (also `--layers --notes --blocks --layouts --describe`) |
| `export` | `$CLI export part.vkd part.step` (extension picks the format: `.dxf` `.pdf` `.step` `.stl` `.obj`) |
| `import` | `$CLI import drawing.dxf --save-as drawing.vkd` (also `.dwg` and `.step`) |
| script | `$CLI new --run script.vks --save-as out.vkd` — a `.vks` file, one input line per row (AutoCAD `.scr` semantics) |

`--exec` runs ONE complete command per flag, in order, left to right; the
first failure aborts with `E_EXEC`. `--run` feeds a script file where a
command may span several lines (see §2 on when you need that).

### 1b. Live IPC — drive the running GUI over the `vikicad` local socket

Start the GUI (from an agent sandbox, ALWAYS as a systemd user unit —
anything else gets reaped with the sandbox):

```sh
systemd-run --user --unit=vikicad-gui --collect \
    --setenv=QT_QPA_PLATFORM=xcb ${DISPLAY:+--setenv=DISPLAY="$DISPLAY"} \
    /path/to/vikicad/build/debug/gui/vikicad
# when done:  systemctl --user stop vikicad-gui
```

| Method | One-line example | Effect |
|---|---|---|
| `ping` | `$CLI connect ping` | `{"pong":true}` — poll this after starting the unit |
| `exec` | `$CLI connect exec "CIRCLE 50,50 10"` | run any command; result carries `messages` |
| `query` | `$CLI connect query bounds` | kinds: `entities` (default), `layers`, `bounds`, `notes`, `blocks`, `layouts`, `describe` (§4), `ui` |
| `open` | `$CLI connect open part.vkd` | File>Open dispatch by extension: `.vkd` `.dxf` `.dwg` `.step` |
| `save` | `$CLI connect save out.vkd` | save the live document |
| `export` | `$CLI connect export out.step` | File>Export by extension: `.step` `.dxf` `.stl` `.obj` |
| `screenshot` | `$CLI connect screenshot shot.png` | 2D canvas grab, or the OCCT framebuffer when in 3D |
| `view3d` | `$CLI connect view3d on` | toggle the 3D view (`on`/`off`); returns `is3d` |
| `viewdir` | `$CLI connect viewdir FRONT` | aim the 3D camera along `TOP BOTTOM FRONT BACK LEFT RIGHT ISO` and refit the scene (3D view only — §5a) |
| `pick3d` | `$CLI connect pick3d 400 300` | click at physical view pixels; no coords = view centre; returns e.g. `"picked face 2 of solid #3"` |
| `sketchface` | `$CLI connect sketchface` | start a sketch on the currently selected face (pick3d a face first) |
| `insertstep` | `$CLI connect insertstep other.step` | additive STEP import as an assembly component |

Everything registered in the single CommandProcessor is available through
BOTH channels — a command that works in `--exec` works in `connect exec`
and in the GUI command line, unchanged.

---

## 2. Command grammar

Commands are state machines fed by whitespace-separated tokens. The rules:

1. **Numeric parameters come BEFORE entity selection.** The entity-set
   stage is greedy: it swallows every remaining numeric token on the line.
   That is why every command reads `HOLE <diameter> <depth|T> <center> <solidId>`
   and never `HOLE <solidId> <diameter> ...`. Keep this order in mind when
   reading prompts.
2. **Keywords** are case-insensitive words at keyword prompts, usually with
   a one-letter short form: `EXTRUDE 10 Cut`, `HOLE 8 T ...` (T = through),
   `INSPECT 2 Faces`, `SECTION XY 5 2`. An empty line (or a lone click in
   the GUI) at a keyword prompt accepts the `<default>`.
3. **Points** are `x,y` in mm on the current work plane. Prefixes/suffixes:
   - `@dx,dy` — relative to the last point;
   - `dist<angle` — polar (degrees), `@` makes it relative;
   - units: `25.4`, `25.4mm`, `1in`, `1"` are all one inch.

   Verified example (a 40x30 L outline plus an inch-wide rectangle):

   ```sh
   $CLI new --exec "LINE 0,0 @40,0 @30<90 0,30" \
            --exec "RECT 50,0 @1in,20mm" --save-as gram.vkd
   ```
4. **One `--exec` = one complete command.** In strict mode a command left
   waiting for input is an error (`incomplete input for command X`) —
   except that up to three implicit Enters are applied first, so trailing
   optional stages (LINE's "next point", keyword defaults) self-finish.
5. **Multi-line commands need a script.** When two consecutive stages both
   read entity ids (e.g. EXTRUDE Cut: target, then profiles), a single line
   cannot separate them — the greedy gulp hands everything to the first
   stage. Put each answer on its own `.vks` line:

   ```sh
   printf 'EXTRUDE 10 CUT\n2\n3\n' > cut.vks    # target=2, profile=3
   $CLI new --exec "RECT 0,0 40,30" --exec "EXTRUDE 10 1" \
            --exec "CIRCLE 30,15 4" --run cut.vks --save-as cut.vkd
   ```

Useful command vocabulary (aliases in parentheses): 2D drawing
`LINE (L) CIRCLE (C) ARC (A) RECT PLINE (PL) ELLIPSE (EL) SPLINE (SPL)`,
editing `MOVE (M) COPY (CO) ROTATE (RO) MIRROR (MI) SCALE (SC) ERASE (E)
TRIM (TR) OFFSET (O) FILLET (F) UNDO (U) REDO`, 3D
`WORKPLANE (WP) EXTRUDE (EXT) REVOLVE (REV) UNION SUBTRACT (SUB)
INTERSECT (INT) HOLE (HO) SHELL (SH) SPLIT COMBINE SWEEP (SW) LOFT (LO)
FILLET3D (F3D) CHAMFER3D (CH3D) MOVE3D (M3) ROTATE3D (RO3)`, and the
index-addressed set in §3.

---

## 3. Sub-shape addressing: INSPECT indices + the parity commands

Headless agents reference faces and edges by **deterministic index**: the
0-based `TopExp_Explorer` order over the solid's shape. The indices are
stable for a given shape, but **recompute them after every operation that
rebuilds the shape** (hole, fillet, feature edit... — see the worked
example below where they reshuffle twice).

`INSPECT <solidId> [Faces/Edges/All]` (alias `INS`) is the discovery verb.
One message per sub-shape:

```
face 6: cylinder r=5.0 area=314.2 centroid=(20.0,15.0,5.0)
edge 13: circle r=5.0 len=31.4 mid=(15.0,15.0,10.0)
```

Surface types: `plane`, `cylinder r=`, `cone`, `sphere r=`, `torus`,
`freeform`; curve types: `line`, `circle r=`, `ellipse`, `spline`. All
numbers are fixed one-decimal — match faces by type + centroid, edges by
type + midpoint.

The index-addressed commands (numeric size first, then one positional
gulp `solidId index [index...]`):

| Command | Form | Does |
|---|---|---|
| `FEATEDIT` | `FEATEDIT <param> <value> <nodeIndex> <solidId>` — params `diameter depth centerx centery height thickness`; `FEATEDIT LIST <id>` discovers them | edit feature-history parameters (the Properties panel, headless) |
| `PUSHPULL` (PP) | `PUSHPULL <distance> <faceIndex> <solidId>` | move a planar face: +boss / −pocket |
| `SHELLOPEN` | `SHELLOPEN <thickness> <solidId> <faceIndex> [faceIndex...]` | hollow the solid, listed faces removed (open) |
| `SPLITFACE` | `SPLITFACE <toolSolidId> <toolFaceIndex> <targetSolidId>` | cut the target by another solid's face (planar or curved) |
| `FILLETEDGES` (FEDGES) | `FILLETEDGES <radius> <solidId> <edgeIndex> [edgeIndex...]` | round the listed edges |
| `CHAMFEREDGES` (CEDGES) | `CHAMFEREDGES <distance> <solidId> <edgeIndex> [edgeIndex...]` | bevel the listed edges |
| `MATE` | `MATE <movingId> <movingFaceIdx> <fixedId> <fixedFaceIdx>` | snap a planar face flat+centred onto another, normals opposed |
| `DRAFT` | `DRAFT <angleDeg> <solidId> <faceIndex> [faceIndex...]` | tilt faces for mold pull (+Z pull, neutral plane at the solid's zMin) |

### Worked end-to-end example (every line ran, outputs shown are real)

Box → hole → edit the hole's diameter → fillet the top rim → export STEP.

```sh
# 1. A 40 x 30 x 10 box. RECT is entity 1; EXTRUDE consumes it -> solid id 2.
$CLI new --exec "RECT 0,0 40,30" --exec "EXTRUDE 10 1" --save-as part.vkd
#   {"ok":true,"result":{...,"messages":["solid 2 created (height 10)"],...}}

# 2. Discover the geometry. 6 faces / 12 edges; top face is
#    "face 5: plane area=1200.0 centroid=(20.0,15.0,10.0)".
$CLI open part.vkd --exec "INSPECT 2 All"

# 3. An 8 mm through-hole at the centre. NOTE the message: HOLE replaced
#    the box under a NEW id -> "hole (d=8) in solid 3".
$CLI open part.vkd --exec "HOLE 8 T 20,15 2" --save

# 4. Discover the editable feature parameters, then widen the bore.
$CLI open part.vkd --exec "FEATEDIT LIST 3"
#   messages: ["solid 3: 3 editable parameter(s)","hole 1: diameter = 8.0",
#              "hole 1: center x = 20.0","hole 1: center y = 15.0"]
$CLI open part.vkd --exec "FEATEDIT diameter 10 1 3" --save
#   messages: ["solid 3: diameter = 10.0 on node 1"]

# 5. Re-INSPECT — the regeneration RESHUFFLED the indices. The top rim is
#    now the four "line ... mid=(...,10.0)" edges: 3, 6, 9, 12
#    (the circle at z=10 is edge 13 — don't fillet the bore rim here).
$CLI open part.vkd --exec "INSPECT 3 All"

# 6. Fillet the top rim, verify (4 new r=2.0 cylinder faces), export.
$CLI open part.vkd --exec "FILLETEDGES 2 3 3 6 9 12" --save
#   messages: ["filleted 2 mm on 4 edge(s) of solid 3"]
$CLI open part.vkd --exec "INSPECT 3 Faces"        # 11 faces now
$CLI export part.vkd part.step
#   {"ok":true,"result":{"savedTo":"part.step","sidecarNotes":0,"solids":1}}
head -1 part.step                                   # ISO-10303-21;
```

---

## 4. The verification loop

Never trust a mutation you did not observe. After each step:

0. **DESCRIBE / `query describe`** — the "understand the model" view, one
   call, two renderings of the SAME computed data.

   Text (`DESCRIBE`, alias `DESC`; add a solid id to scope to that solid):
   one grep-friendly line per fact — document (units/entity/layer counts),
   each solid (volume mm³, area mm², bbox, centroid) with its feature
   history indented under it, each sketch (plane + entity count), each
   layer (2D entity counts by type). Verified on the §3 part after step 3:

   ```sh
   $CLI open part.vkd --exec "DESCRIBE"
   #   messages: ["document: units=mm entities=1 layers=1",
   #     "solid 3: volume=11497.3 mm3 area=3950.8 mm2 bbox=(0.0,0.0,0.0)-(40.0,30.0,10.0) centroid=(20.0,15.0,5.0)",
   #     "  base 0",
   #     "  hole 1 d=8 through @(20,15)",
   #     "layer '0': 2d=0"]
   ```

   Machine (`query describe`): the same data as structured JSON — numbers
   as numbers, `features[]` carries the param-bearing nodes (extrude/hole/
   shell) with flattened name/value pairs, and **no brep base64 anywhere**
   (unlike `query entities`, which embeds each solid's brep — prefer
   `describe` whenever you only need to understand the model). Both query
   paths serve it: offline `$CLI query part.vkd --describe`, live
   `$CLI connect query describe`. Verified output (offline, same part):

   ```json
   {"ok":true,"result":{"count":1,"describe":{
     "units":"mm","entityCount":1,"layerCount":1,
     "solids":[{"id":3,"component":"",
       "volume":11497.345175425633,"area":3950.7964473723105,
       "bbox":{"min":[-1e-07,-1e-07,-1e-07],
               "max":[40.0000001,30.0000001,10.0000001]},
       "centroid":[20.0,15.0,5.0],
       "features":[{"node":1,"kind":"hole","diameter":8,"through":true,
                    "center":[20,15]}]}],
     "sketches":[],"layers":[{"name":"0","count":0,"counts":{}}]},
   "file":"part.vkd"}}
   ```

   (The 1e-7 bbox fringe is the OCCT `Bnd_Box` gap — the text view rounds
   it away; treat it as exact-zero when matching.)
1. **Counts and bounds** — `$CLI query part.vkd --bounds` offline, or
   `$CLI connect query entities` / `query bounds` live. Entity count and
   bounding box catch "the part vanished" instantly.
2. **LIST / INSPECT** — `LIST <x,y>` (LI) picks the entity at a point and
   dumps it (verified: `LIST 20,0` → `#1  line  layer '0'  bounds 40.00 mm
   x 0.00 mm` + its JSON). On a **solid**, LIST adds the quick-numbers line
   (same shared computation as DESCRIBE — verified on the §3 part after
   the hole):

   ```
   #3  solid  layer '0'  bounds 40.00 mm x 30.00 mm
   volume=11497.3 mm3 area=3950.8 mm2 bbox=(0.0,0.0,0.0)-(40.0,30.0,10.0) features=2
   ```

   (`features=` counts the history nodes — base + hole here; 0 = plain
   brep with no history.) `INSPECT` proves topology: face/edge counts, surface types,
   areas, centroids. A fillet shows up as new `cylinder` faces; a shell as
   a face-count jump (6 → 10 for an open-top-and-bottom box).
3. **Numeric checks** — all verified forms:

   ```sh
   $CLI open pair.vkd --exec "MEASURE3D 2 4"    # "min distance = 15 mm"
   $CLI open pair.vkd --exec "CLASH 2 4"        # "#2 and #4 do not interfere"
   $CLI open pair.vkd --exec "CLASH"            # sweep every solid pair
   $CLI open pair.vkd --exec "SECTION XY 5 2"   # "#2 section area = 400.000 mm²"
   ```

   (`MEASURE3D`/`M3D` min distance between two solids; `CLASH`/`INTERFERE`
   overlap volume; `SECTION XY|XZ|YZ <offset> <id>` cross-section area.)
4. **Screenshot + image diff** (live GUI only) — `connect view3d on`,
   `connect screenshot a.png`, mutate, screenshot again, compare. Use a
   dhash distance for "must be identical" (undo/redo) and a
   changed-pixels percentage for "must differ" (a hole appeared).
   **`scripts/gui-smoke.sh` is the reference implementation** of this
   whole loop — thresholds, PIL one-liners, JSON helpers, the systemd
   start/stop recipe. Extend it when you add an IPC-drivable feature.

---

## 5. Seeing the model

Three ways to LOOK at the part, cheapest first. As everywhere in this
guide, every command below ran verbatim; outputs shown are real (a
40x30x10 box with an 8 mm through-bore, built in §5d).

### 5a. Live eyes — `viewdir` + `screenshot` (GUI over IPC)

`viewdir` aims the 3D camera along a standard view and refits the whole
scene, so the following `screenshot` is always framed. The loop:

```sh
$CLI connect open "$PWD/see.vkd"
$CLI connect view3d on           # viewdir errors while the 2D canvas is up
$CLI connect viewdir FRONT       # {"ok":true,...,"view":"FRONT"}
$CLI connect screenshot front.png
$CLI connect viewdir ISO
$CLI connect screenshot iso.png
```

Verified: the FRONT dump shows the plate edge-on (bore invisible), the ISO
dump shows it in 3/4 with the bore — and `gui-smoke.sh` measures a 308/1024
dhash distance between the ISO and TOP renders (the camera really moved).
Wrong names fail loudly: `viewdir NOPE` → `unknown view: NOPE (try
TOP/BOTTOM/FRONT/BACK/LEFT/RIGHT/ISO)`.

### 5b. Offline eyes — `MAKEVIEW` (HLR): a projection you QUERY, no GUI

`MAKEVIEW <VIEW> <solidId>` runs hidden-line removal and adds every
*visible* projected edge as a 2D LINE entity — a "drawing" made of numbers
instead of pixels:

```sh
$CLI open see.vkd --exec "MAKEVIEW TOP 3" --save-as proj.vkd
#   messages: ["MAKEVIEW TOP: 36 visible edges from solid 3"]
$CLI query proj.vkd --entities   # filter "type":"line" -> 36 segments
```

Those 36 lines are the 4-edge outline plus the bore circle chorded into 32
segments — countable, measurable proof the hole exists, from a shell with
no display at all. Caveats (verified): hidden edges are dropped (v1), and
the lines land in the VIEW's own 2D frame, which can be offset/mirrored
from the model's XY footprint — measure the lines' own bounds, don't
overlay them on the plan.

### 5c. External eyes — export a mesh for outside analysis

```sh
$CLI export see.vkd see.stl   # {"format":"binary","savedTo":...,"solids":1}
$CLI export see.vkd see.obj   # {"faces":120,"savedTo":...,"vertices":130}
```

Feed the STL/OBJ to any external mesh tool (or a few lines of Python) when
you need analysis VikiCAD does not do itself.

### 5d. Worked perception → act → verify loop (all output real)

```sh
# 0. Build the part and PERCEIVE the baseline.
$CLI new --exec "RECT 0,0 40,30" --exec "EXTRUDE 10 1" --save-as see.vkd
$CLI open see.vkd --exec "DESCRIBE"
#   "solid 2: volume=12000.0 mm3 area=3800.0 mm2
#    bbox=(0.0,0.0,0.0)-(40.0,30.0,10.0) centroid=(20.0,15.0,5.0)"

# 1. INSPECT for the actionable indices (top face = the plane at z=10).
$CLI open see.vkd --exec "INSPECT 2 Faces"
#   ... "face 5: plane area=1200.0 centroid=(20.0,15.0,10.0)"

# 2. ACT.
$CLI open see.vkd --exec "HOLE 8 T 20,15 2" --save
#   messages: ["hole (d=8) in solid 3"]   <- NEW id, as always

# 3. VERIFY numerically: describe again and check the DIFF. Volume fell
#    12000.0 -> 11497.3, i.e. by pi*4^2*10 = 502.7 = the bore. Area rose
#    (bore wall added more than the two discs removed). History gained
#    the hole node. LIST at a point agrees (same shared metrics).
$CLI open see.vkd --exec "DESCRIBE 3"
#   "solid 3: volume=11497.3 mm3 area=3950.8 mm2 ..." + "  hole 1 d=8 through @(20,15)"
$CLI open see.vkd --exec "LIST 20,0"
#   "volume=11497.3 mm3 area=3950.8 mm2 bbox=(0.0,0.0,0.0)-(40.0,30.0,10.0) features=2"

# 4. VERIFY visually (GUI running, §1b recipe): look at it.
$CLI connect open "$PWD/see.vkd"
$CLI connect view3d on
$CLI connect viewdir ISO
$CLI connect screenshot iso.png   # bore clearly visible in the dump
```

Rule of thumb: numbers first (steps 0–3 need no GUI and never lie),
pixels last (step 4 catches what numbers cannot — a part rendered black,
an assembly exploded across the scene).

---

## 6. Gotchas

- **`H` is HATCH, not HOLE.** `HO` is the HOLE alias. Verified:
  `H 12 T 20,15 1` fails with `invalid number: T` because HATCH expects a
  pattern/scale, not hole parameters.
- **A negative extrusion is NOT a hole.** `EXTRUDE -10` builds material
  *below* the plane (verified: faces at z=0..−10). Material removal is
  `HOLE` (round bores, feature-tree editable) or `EXTRUDE <h> CUT` with a
  script (§2 rule 5, arbitrary profiles).
- **Mutating ops may replace the entity id.** HOLE turned solid 2 into
  solid 3; SPLITFACE/SPLIT replace the target with new pieces; EXTRUDE
  Cut/Join produce a new id too. Re-run `query entities` (filter
  `"type":"solid"`) or read the command's message instead of caching ids.
- **Sub-shape indices reshuffle** whenever the shape is rebuilt —
  re-INSPECT before every index-addressed call (§3).
- **`ok:true` does not mean the command did something.** A command that
  cancels itself (bad id, refused geometry) still returns
  `{"ok":true,...}` with the explanation only in `messages`
  (e.g. `"that id is not a solid"`). Always check `messages`; in
  `gui-smoke.sh` see how every `gexec` is followed by a state assertion.
- **DWG is import-only.** `$CLI import file.dwg --save-as out.vkd` works
  (R14–2013 natively, 2018+ via a bundled `dwg2dxf` fallback); there is no
  DWG export — export DXF instead.
- **GUI from a sandbox: systemd unit or nothing.** A GUI launched with
  plain `&`/`nohup` from an agent shell dies with the sandbox. Use the
  `systemd-run` recipe in §1b, poll `connect ping`, and stop the unit when
  done.
- **MATE needs two planar faces**, and **PUSHPULL a planar face** — INSPECT
  shows each face's surface type before you commit.
- **3D screenshots are PPM bytes whatever the extension.** In the 3D view
  the OCCT framebuffer dump ignores the `.png` suffix (verified: the file
  starts `P6`). PIL/`gui-smoke.sh` open it by content and don't care;
  stricter tools do — convert first (`python3 -c "from PIL import Image;
  Image.open('iso.png').save('iso_real.png')"`). The 2D canvas grab is a
  real PNG.

---

## 7. Gerber kits headless (G1)

Open a fabrication-output directory (or a LONE Gerber/Excellon file — the
content sniff routes it through the same importer) and query it, offline or
against the live GUI. Every line below RAN; outputs are real (trimmed).

```bash
# Offline batch: directory -> .vkd. One layer per recognized file, whole kit
# in ONE transaction, reports/support files skipped by content sniff.
$CLI import /home/lex/computer/pcb-ref/S5M0PCBA --save-as kitA.vkd
#  {"ok":true,"result":{"entities":2572,"files":14,
#    "layers":["Bottom-Copper","Top-Copper",...,"Mech-15","Outline","Drill",
#              "Drill-NPTH"],   <- paint order: copper first, drills last
#    "skipped":["S5M0PCBA1.GKO: no graphical objects (empty layer skipped)",
#               "Status Report.Txt: not a Gerber/Excellon file (content sniff)",
#               ...],"warnings":[]}}
# PCBA's Outline comes from GM13 (GKO/GM1 are header-only): the outline
# election takes the best of GKO > GM1 > GM13 that LOOKS like a contour
# (has strokes + spans >= 60 % of the board on one axis). On PCBB the GKO
# is a filled antenna keepout: it stays "Keepout" and paints BELOW copper.

$CLI query kitA.vkd --layers   # colors: Top-Copper #e53935, Outline #ff00ff...
$CLI query kitA.vkd --bounds
#  {"ok":true,"result":{"bounds":[-0.0127,-20.0127,90.0749,30.8445],
#    "count":2572,...}}   <- mm, always

# Live GUI (start the unit as in §1b, then):
$CLI connect open /home/lex/computer/pcb-ref/S5M0PCBB   # {"ok":true,...}
$CLI connect query entities   # count: 3075
$CLI connect open /home/lex/computer/pcb-ref/S5M0PCBB/S5M0PCBB1.TXT
$CLI connect query entities   # count: 330 <- lone drill file = 330 circles
$CLI connect screenshot /tmp/drills.png
# UNDO once = the whole kit vanishes; REDO restores it (single transaction).
```

Gotchas: `connect open` REPLACES the document (fresh Document per kit, like
File > Open); entity `extra()` keeps `gpol:"C"` on clear-polarity (LPC)
objects and `plated:true/false` on drill hits; independent ground truths for
assertions live next to the kits (.DRR = hole counts per tool, .REP = used
D-codes). Reference render diff: `scripts/gerber-ref-diff.sh` (SKIPs until
gerbv is installed).
