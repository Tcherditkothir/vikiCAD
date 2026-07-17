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
| `export` | `$CLI export part.vkd part.step` (extension picks the format: `.dxf` `.pdf` `.step` `.stl` `.obj`, fab extensions `.gtl`...`.gko`/`.gbr`/`.txt` for one layer — §7d; a DIRECTORY target exports the whole Gerber kit) |
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
| `open` | `$CLI connect open part.vkd` | File>Open dispatch by extension: `.vkd` `.dxf` `.dwg` `.step`; Gerber kit dir or lone fab file too. Importer warnings (e.g. valid-but-empty fab file) come back in the reply's `warnings` array |
| `save` | `$CLI connect save out.vkd` | save the live document |
| `export` | `$CLI connect export out.step` | File>Export by extension: `.step` `.dxf` `.stl` `.obj`, fab extensions (one layer, optional layer-name arg), directory = Gerber kit (§7d) |
| `screenshot` | `$CLI connect screenshot shot.png` | 2D canvas grab, or the OCCT framebuffer when in 3D. Add `clean` (`… screenshot shot.png clean`) for the 2D document WITHOUT overlays (grid/UCS/crosshair) — image-diff friendly, works even while the 3D view is up |
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
objects, `dcode:N` on aperture-painted objects, and `plated:true/false` +
`tool:"Tn"` on drill hits (see §7c for the whole inspection kit);
independent ground truths for assertions live next to the kits (.DRR =
hole counts per tool, .REP = used D-codes). Reference render diff:
`scripts/gerber-ref-diff.sh` (SKIPs until gerbv is installed).

### 7a. The layer stack (G2): LAYER + BOARDVIEW

Every layer carries three CAM properties, persisted in `.vkd` and visible
in `query layers` (`alpha`, `rank`, `gerberRole`) and in the GUI LayerPanel
(Alpha / Rank / Role columns, ▲▼ buttons, right-click "Set Gerber role"):

- **alpha** — compositing opacity 0..100 (100 = opaque default). Applied to
  the layer's composite as a whole, so a faded plane fades evenly.
- **rank** — paint order, LOWER paints first. Ties keep document draw
  order, so an all-rank-0 document renders exactly as before. The kit
  importer stamps its paint ranks here (copper 10/20 ... outline 90,
  drills 95/96).
- **gerberRole** — reassignable CAM role (`Copper-Top`, `Copper-Bottom`,
  `Mask`, `Silk`, `Paste`, `Outline`, `Drill`, `Mech`; empty = none).
  Assigning a role RECOLORS the layer to the role palette and moves it to
  the role rank — the escape hatch when the outline election guessed wrong.

Both channels, verified verbatim (offline `--exec` and live `connect exec`):

```sh
$CLI connect exec "LAYER Top-Copper ALPHA 30"   # 0..100, Enter = 100
$CLI connect exec "LAYER Mech-15 ROLE Outline"
#   messages: ["layer 'Mech-15' role = Outline (color #ff00ff, rank 90)"]
$CLI connect exec "LAYER Bottom-Copper UP"      # one slot later in the stack
#   messages: ["layer 'Bottom-Copper' moved up (painted later, on top)"]
$CLI connect exec "LAYER Bottom-Copper RANK 12" # absolute rank, if you prefer
$CLI connect exec "LAYER Nope ALPHA 10"
#   ok:true BUT messages: ["no layer named 'NOPE'"] — check messages!
```

`BOARDVIEW TOP|BOTTOM|ALL` (alias `BV`, GUI: View > Board view) is the CAM
stack preset: TOP dims the bottom-side layers to 25 %, BOTTOM dims the top
side AND mirrors the view left-right (the classic solder-side view — done
at camera level, so picking/snapping still work and bottom-silk text reads
normally), ALL restores everything (verified: the ALL clean capture is
byte-identical to the pre-BOARDVIEW one). Side classification: gerberRole
first (Copper-Top/Copper-Bottom; Outline + Drill never dim), layer-name
prefix (Top-/Bottom-) for sideless roles. The alphas it sets are ordinary
layer alphas (persisted if you save); the mirror is view state only and is
reset when another document is opened. Headless (no view) the mirror step
reports "no view attached" but the alphas still apply.

```sh
$CLI connect exec "BOARDVIEW TOP"
#   ["board view TOP: 5 bottom-side layer(s) dimmed to 25%"]
$CLI connect exec "BOARDVIEW BOTTOM"
#   [...,"view mirrored left-right (solder side)"]
$CLI connect exec "BOARDVIEW"        # bare = ALL (the default)
```

### 7b. Measuring on gerbers (G2): MINDIST + DIM, CAM semantics

`MINDIST <idA> <idB>` (alias `MD`; also honors a pre-selected pair) is the
CAM clearance measurement: minimum **edge-to-edge** distance with material
semantics — a wide trace counts its round-capped footprint (width/2 off
each side of the centerline), a circle/drill counts its radius, a flashed
pad (insert of a `GBR-*` block) counts the block's REAL aperture footprint
through the insert transform, a region/pour its filled rings. Entities
that cannot be reduced to strokes (text...) fall back to their bounding
box and the output SAYS so (`method` flips from `"exact"` to `"bbox"`,
plus a `"#id measured on its bounding box"` note). Both channels, same
line (offline `--exec` shown; `connect exec` identical — verified).

```sh
$CLI open kitA.vkd --exec "MINDIST 2391 2392"   # two real drill hits
#  messages:
#   ["min edge-to-edge distance #2391 -> #2392 = 7.5104 mm (exact)",
#    "closest points: (24.0076, -8.9503) -> (24.3923, -1.4498) mm",
#    "{\"mindist\":{\"a\":2391,\"b\":2392,\"method\":\"exact\",
#      \"mm\":7.510364123916017,\"overlap\":false,
#      \"pa\":[24.007634...,-8.950267...],\"pb\":[24.392319...,-1.449762...]}}"]
```

Agent contract: the LAST message that starts with `{` is a compact JSON
trailer — `mindist.mm` (mm, full precision), `overlap` (touching or
overlapping material reports `mm: 0, overlap: true`), `method`
(`exact`/`bbox`), `pa`/`pb` (the two closest material-edge points, mm).
Parse that, not the human lines. Verified against the by-hand value
`|c1-c2| - r1 - r2 = 7.510364123916017` from the same drills' `query
entities` data; pads work too (`MINDIST 376 377` → `14.8007 mm (exact)`
between two GBR-D67 flashes). Multi-ring macro footprints (Altium
RoundedRect = overlapping rects + corner disks) are measured as the
UNION of their rings — an entity fully buried inside such a pad reports
`overlap:true, mm:0`, not a distance to an internal seam. Known bias
(PCB_CAM.md debt): round flashes are baked as polygons INSCRIBED in the
circle, so pad-to-pad clearances in oblique directions read up to
~0.002 mm LARGER than physical truth. In the GUI the result also draws a
dashed witness line + end ticks (dark halo under the dashes, readable at
board scale) on the canvas until the next command starts.

Dimensioning pad-to-pad: every insert's flash origin is BOTH an Endpoint
and a **Center** snap (so the Center osnap alone reaches pad centers in
the GUI — note this applies to EVERY block insert, not just `GBR-*`
pads: deliberate, see LESSONS 2026-07-17); headless you pass the centers
straight from `query entities` (`geom.pos` of the inserts). Verified on the real kit:

```sh
$CLI open kitA.vkd \
     --exec "DIMALIGNED 83.134962,21.26996 80.635094,6.26999 85,14" \
     --save-as kitA_dim.vkd
#  entityCount 2572 -> 2573; the new entity is
#  {"type":"dimension","geom":{"kind":1,"a":[83.134962,21.26996],
#   "b":[80.635094,6.26999],"pos":[85,14],...}}  (aligned = 15.207 mm)
```

### 7c. Inspecting gerber objects (G2): APERTURES, DRILLREPORT, SELECT

Every fab-file entity knows WHAT it is. The importers tag the extra JSON of
each entity (`dcode:N` = the aperture that painted it, on traces/arcs/pads;
`tool:"Tn"` + `plated` on drill hits; G36/G37 regions have no aperture so no
dcode) and persist the file-level tables on the LAYER (`camMeta`, saved in
`.vkd`, visible as `cam` in `query layers`):

```sh
$CLI query kitA.vkd --layers
#  ...,"name":"Top-Copper","cam":{"apertures":{
#    "D10":{"desc":"Circle d=0.200","params":[0.199898],"shape":"Circle","usage":126},
#    "D15":{"desc":"RoundedRect 0.600x0.900 r=0.054 rot 270deg",
#           "macro":"ROUNDEDRECTD15","params":[],"shape":"Macro","usage":38},...}}
#  ...,"name":"Drill","cam":{"tools":{
#    "T1":{"dia":0.299974,"plated":true,"usage":64},...}}
```

Shapes: `Circle`/`Rect`/`Obround`/`Polygon`/`Macro`; `params` are mm; a
macro's `desc` uses Altium's `G04:AMPARAMS|...` comment when present (the
designer-level truth: RoundedRect sizes, corner radius, rotation), else
`Macro <name> ~WxH`. `usage` counts painting OPERATIONS (a coalesced
polyline still counts each stroke). Layers with macro apertures also carry
`cam.macros` (`{name: [[code, params...], ...]}`, primitives in mm) — the
%AM bodies the G3 RS-274X writer re-emits verbatim (GerberWriter.h). A kit opened into a virgin document
also DROPS the default layer "0" (pure CAM stack; the current layer becomes
the first kit layer — that is why `query layers` shows no `"0"` above).

**APERTURES [layer]** (alias `APER`; bare = every layer that has a table)
prints the aligned table + the `{`-trailer (same contract as MINDIST):

```sh
$CLI open kitA.vkd --exec "APERTURES Top-Silk"
#  messages: ["apertures on 'Top-Silk' (5 D-code(s), 5 used)  [role Silk]",
#   "  D10  Circle d=0.200  uses 27",
#   "  D11  Circle d=0.127  uses 24", ... ,
#   "  D14  Circle d=0.160  uses 983",
#   "{\"apertures\":{\"Top-Silk\":{\"D10\":{...,\"usage\":27},...}}}"]
```

**DRILLREPORT** (alias `DR`) is the hole table by diameter+plating over the
LIVE drill circles (erase a hole, the count drops). Verified on S5M0PCBA —
the totals ARE the kit's .DRR report (182 = 180 PTH + 2 NPTH), which the
test suite compares tool by tool on both kits. The G3 Excellon writer
(ExcellonWriter.h) regenerates its Tn table from the same live circles
(grouped at 1e-4 mm, plated first) and writes METRIC with explicit decimal
coordinates — export -> re-import reproduces this report exactly:

```sh
$CLI open kitA.vkd --exec "DRILLREPORT"
#  ["drill report: 182 hole(s) — 180 plated, 2 NPTH, 9 diameter group(s)",
#   "  d=0.300 mm  plated  64 hole(s)  T1 on Drill", ...,
#   "  d=2.400 mm  NPTH    2 hole(s)   T9 on Drill-NPTH", ...,
#   "{\"drillreport\":{\"npth\":2,\"plated\":180,\"rows\":[...],\"total\":182}}"]
```

**SELECT [id...]** (alias `SEL`; bare = clear) replaces the selection set
headlessly — it feeds every pickfirst behavior (MINDIST's pre-selected
pair, ERASE...) and, in the GUI, the Properties panel. Selected gerber
entity -> the panel grows a read-only inspector section; `query ui` now
returns the visible rows as `propRows`, so an agent can read the panel.
Executed against the live GUI (pad #773 is a GBR-D15 flash):

```sh
$CLI connect exec "SELECT 773"     # ["1 entity(ies) selected"]
$CLI connect query ui              # propRows (key/value), verbatim:
#   type            | insert
#   id              | 773
#   gerber D-code   | D15
#   gerber aperture | RoundedRect 0.600x0.900 r=0.054 rot 270deg
#   gerber polarity | dark
#   layer role      | Copper-Top (Top-Copper)
#   block           | GBR-D15  (then pos / rotation / scale)
```

Drill hits show `drill tool` (`T3  d=0.700 mm`) and `drill plating`
(`plated (PTH)` / `non-plated (NPTH)`) instead; LPC objects show
`gerber polarity | clear (LPC, erases below)`. All three commands run
through the single CommandProcessor: identical in `--exec`, `connect exec`
and the GUI command line (both channels exercised by gui-smoke).

### 7d. Editing + exporting fab files (G3): the whole CAM loop headless

The point of the chantier: EDIT a fabrication kit and SHIP the files. Every
line below ran verbatim; outputs are real (trimmed).

**CAM editing commands** (same grammar laws as everything else — numeric
parameters BEFORE the greedy entity set):

```sh
$CLI open kitA.vkd --exec "PLWIDTH 0.42 262"     # trace edit (alias PLW)
#   ["width 0.42 mm set on 1 polyline(s)"]  <- the RS-274X writer will
#   regenerate a C,0.42 aperture for it (an UNEDITED trace re-emits its
#   ORIGINAL definition instead, rect apertures included)
$CLI open kitA.vkd --exec "LAYER Drill CURRENT"  # headless LayerPanel click
#   ["layer 'Drill' is now current (new entities land here)"]
# ... then "CIRCLE 45,-10 0.55" = a NEW 1.1 mm plated hole on Drill.
# DRILLREPORT counts it right away ("d=1.100 mm  plated  1 hole(s)  new on
# Drill" — 'new' = drawn here, no source tool yet); plating defaults to the
# layer role (Drill-NPTH => NPTH), exactly like the Excellon writer.
```

**Export — offline CLI.** A DIRECTORY target writes the whole kit
(`<vkd-base>.<EXT>`, extension from each layer's CAM role + Top-/Bottom-
side; every Drill-role layer grouped into ONE .TXT with PLATED/NON_PLATED
sections); a fab extension writes one layer:

```sh
$CLI export kitA.vkd fab/          # kit -> fab/kitA.GTL .GBL .GTS .GBS
#   .GTO .GBO .GTP .GBP .GKO .TXT; result lists per-file {path, layers,
#   entities, drill}, plus skippedLayers (real output:
#   "Top-Pads: no kit extension for role 'none'...", "Mech-15: ...") and
#   the writers' warnings.
$CLI export kitA.vkd top.GTS       # extension resolves the layer (unique)
#   {"files":[{"drill":false,"entities":184,"layers":["Top-Mask"],...}],...}
$CLI export kitA.vkd holes.TXT     # several drill layers -> ONE Excellon
#   {"drill":true,"holes":182,"layers":["Drill","Drill-NPTH"],"tools":9,...}
$CLI export kitA.vkd mech.GBR --layer Mech-15   # .gbr/.ger need --layer
```

**Export — live GUI over IPC** (File > Export > "Gerber kit (directory)..."
/ "Gerber/Excellon layer..." drive the same code; writer warnings land in
the history bar AND in the reply):

```sh
$CLI connect export "$PWD/live"              # directory = whole kit
#   {"message":"Gerber kit: 10 file(s) → /tmp/.../live (board.GBL, ...)"}
#   (base name = the document's file base, "board" for an unsaved kit)
$CLI connect export "$PWD/live-top.GTS"      # layer resolved by extension
$CLI connect export "$PWD/live-mech.GBR" Mech-15   # explicit layer name
```

**PANELIZE cols rows pitchX pitchY** (alias PNL) duplicates the fab content
(every entity on a layer with a CAM role, drills included — clones keep
their dcode/gpol/tool/plated tags) into a grid; cell (0,0) is the original
board, ONE transaction = one undo. v1 is deliberately simple: no rails, no
mousebites, no %SR (PCB_CAM debt); keep the pitch >= the board size or the
cells overlap. Verified end to end — DRILLREPORT x4 and gerbv ink x4.007 on
the exported panel copper:

```sh
$CLI open kitA.vkd --exec "PANELIZE 2 2 95 55" --exec "DRILLREPORT" \
     --save-as panel.vkd
#   ["panelized 2 x 2 at pitch 95 x 55 mm: 2301 fab entity(ies) -> 4
#     copies (6903 new)",
#    "{\"panelize\":{\"cols\":2,\"created\":6903,\"pitchx\":95,...}}",
#    "drill report: 728 hole(s) — 720 plated, 8 NPTH, ..."]  <- 182 x 4
```

**The DXF<->Gerber bridge**, both directions (LA promesse méca-élec):

```sh
# elec -> meca: a kit crosses DXF with nothing lost — traces stay
# LWPOLYLINEs with their CONSTANT width (code 43), pads stay INSERTs,
# drill hits stay CIRCLEs. Verified: the 926 widths, 182 radii and 1171
# flash positions of kit A re-import equal.
$CLI export kitA.vkd kitA.dxf
#   {"ok":true,"result":{"dxfVersion":"2013","exported":2572,"skipped":0,...}}
$CLI import kitA.dxf --save-as back.vkd    # -> imported: 2572

# meca -> elec: a closed 2D polyline on an Outline-role layer IS a board
# contour — export it as a clean .GKO.
$CLI new --exec "PLINE 0,0 80,0 80,50 0,50 C" --exec "PLWIDTH 0.15 1" \
         --exec "LAYER 0 ROLE Outline" --save-as outline.vkd
$CLI export outline.vkd board.GKO
#   board.GKO = %FSLAX46Y46*% %MOMM*% %ADD10C,0.15*% + the 4 D01 strokes
#   + M02 — parses back as one closed 4-vertex 0.15 mm contour.
```

The truth criterion behind all of this (locked by test_cam_export.cpp +
the gui-smoke `camloop:`/`panel:` phases): export -> re-import brings every
edit back (pad moved 2 mm, width 0.42, erased silk stays gone, new hole
with the right diameter and plating), and gerbv renders the exported files
— not our renderer — identically to their re-export (dhash < 30/1024, ink
delta <= 1 pt) and identically to the ORIGINAL kit when nothing was edited
(observed dhash <= 1, ink delta <= 0.003 pt on all 9 kit-A files).
