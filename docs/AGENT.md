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
| `open` | `$CLI open part.vkd --exec "INSPECT 2 All"` — add `--save` / `--save-as out.vkd` to persist. **`open` dispatches by extension exactly like the GUI's File>Open** (`.vkd` natively; `.dxf` `.dwg` `.step` `.igs` `.stl`; EAGLE `.brd`/`.sch`/`.lbr`; a Gerber kit directory or a lone fab file), so `open file.stl` works wherever `import file.stl` did — it is the same import behind the same routing. The reply carries a `dispatch` field naming the arm taken (`vkd` `dxf` `dwg` `step` `iges` `stl` `eagle` `gerberkit`) plus the `import` verb's counters (`solids` `triangles` `imported` `skipped` `files` `entities` `layers`…). An imported document needs `--save-as` — a bare `--save` is refused, because it would overwrite the SOURCE file with a `.vkd` payload |
| `query` | `$CLI query part.vkd --entities --bounds` (also `--layers --notes --blocks --layouts --describe`) |
| `export` | `$CLI export part.vkd part.step` (extension picks the format: `.dxf` `.pdf` `.step` `.stl` `.obj`, fab extensions `.gtl`...`.gko`/`.gbr`/`.txt` for one layer — §7d; a DIRECTORY target exports the whole Gerber kit) |
| `import` | `$CLI import drawing.dxf --save-as drawing.vkd` (also `.dwg`, `.step` and `.stl`) |
| script | `$CLI new --run script.vks --save-as out.vkd` — a `.vks` file, one input line per row (AutoCAD `.scr` semantics) |
| `anim` | `$CLI anim render --pose p.json --avatar a.json --out DIR` — animated GLB + transparent looping WebP of a kinematic chain (§8; CLI-native, not a CommandProcessor command) |

`--exec` runs ONE complete command per flag, in order, left to right; the
first failure aborts with `E_EXEC` (the message names the failing line).
A `;` inside one `--exec` value separates complete commands —
`--exec "RECT 0,0 40,30; EXTRUDE 10 1"` runs both, still first-failure
aborts. **One invocation = one document**: every `new`/`open` process
starts from a FRESH in-memory document, so state (an open sketch, the
selection, ids just created) never carries across invocations — put the
WHOLE sequence in one call (chain `--exec` flags, `;`-separate them, or
use `--run script.vks`). When you forget, errors say so: `SKETCH CLOSE`
after a sketch was opened in an EARLIER invocation answers "no sketch is
open — each CLI invocation starts from a fresh document...". `--run`
feeds a script file where a command may span several lines (see §2 on
when you need that).

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
| `open` | `$CLI connect open part.vkd` | File>Open dispatch by extension: `.vkd` `.dxf` `.dwg` `.step` `.igs` `.stl`, EAGLE `.brd`/`.sch`/`.lbr`; Gerber kit dir or lone fab file too. Importer warnings (e.g. valid-but-empty fab file) come back in the reply's `warnings` array. **The reply only travels once the whole file finished loading** — on heavy documents raise the wait window first (see §1c below). The offline `open` verb takes the SAME routing (issue #11), so both channels accept the same files |
| `save` | `$CLI connect save out.vkd` | save the live document |
| `export` | `$CLI connect export out.step` | File>Export by extension: `.step` `.dxf` `.stl` `.obj`, fab extensions (one layer, optional layer-name arg), directory = Gerber kit (§7d) |
| `screenshot` | `$CLI connect screenshot shot.png` | 2D canvas grab, or the OCCT framebuffer when in 3D. Add `clean` (`… screenshot shot.png clean`) for the 2D document WITHOUT overlays (grid/UCS/crosshair) — image-diff friendly, works even while the 3D view is up |
| `view3d` | `$CLI connect view3d on` | toggle the 3D view (`on`/`off`); returns `is3d` |
| `viewdir` | `$CLI connect viewdir FRONT` | aim the 3D camera along `TOP BOTTOM FRONT BACK LEFT RIGHT ISO` and refit the scene (3D view only — §5a) |
| `pick3d` | `$CLI connect pick3d 400 300` | click at physical view pixels; no coords = view centre; returns e.g. `"picked face 2 of solid #3"` |
| `sketchface` | `$CLI connect sketchface` | start a sketch on the currently selected face (pick3d a face first) |
| `insertstep` | `$CLI connect insertstep other.step` | additive STEP **or STL** import as an assembly component (the verb name predates STL). The reply names every inserted solid: `inserted[{id,name,triangles}]` + `triangles` total — the mesh triangle count is your MESH2SOLID cost estimate (sewing ≈ one face per triangle, 20k cap); the 3D refresh runs AFTER the reply (deferred, coalesced — issue #7). A heavy mesh may still need a raised wait window (§1c) or the offline `import` |
| `anim` | `$CLI connect anim load pose.json avatar.json [chain.json]` | drive the Timeline dock: `load` (chain resolves like §8 unless given), `frame N` (scrub), `play` / `stop`, `clear`, `status` (JSON state: loaded/frames/frame/playing/keyframes) |

Everything registered in the single CommandProcessor is available through
BOTH channels — a command that works in `--exec` works in `connect exec`
and in the GUI command line, unchanged.

### 1c. The reply wait window: `--timeout` / `VIKICAD_CONNECT_TIMEOUT`

The CLI is a SYNCHRONOUS RPC client: it sends the request, then keeps
waiting for the reply (in chunks) until a window of socket SILENCE
expires — and the GUI answers only once the whole method finished
(`open`/`insertstep` finish when the file fully loaded and its OCC shapes
built; `save`/`export` when everything is serialized). The default
window is 10 s of silence: plenty for every interactive method, but a
100+ MB `.vkd` (112 OCC solids took ~1-5 min here) blows through it and
the caller used to see a FAKE `E_TIMEOUT` while the open was still
progressing fine — issue #10. The window is now tunable per call:

```sh
$CLI connect open big.vkd --timeout 900000     # 15 min, flag anywhere on the line
VIKICAD_CONNECT_TIMEOUT=900000 $CLI connect open big.vkd   # or the env var
$CLI connect --timeout 900000 open big.vkd     # same thing, flag first
```

- Value in MILLISECONDS (`--timeout 900` = 0.9 s). Bad values refuse
  with `E_ARGS` naming the accepted form; unknown `--` options refuse
  too. The env var wins over the flag (export once, forget the flag).
- The window is a SILENCE budget re-armed by every chunk: a big `query
  entities` reply flowing in chunks is never killed mid-transfer; a
  silent/hung GUI still aborts after the window. Timeout errors now say
  so: `no response within 900000 ms (tune with connect --timeout MS or
  VIKICAD_CONNECT_TIMEOUT)`.
- Rule of thumb: interactive methods (`ping exec query screenshot`)
  keep the 10 s default — a timeout there means something is genuinely
  wrong. Heavy whole-document methods (`open` on >50 MB, `save`,
  `export` of a big kit, `insertstep` of a big mesh) deserve a raised
  window sized to the file. A still-running open that outlived your
  window keeps running in the GUI: re-poll with `connect ping` + `query
  entities` to see the document it finished loading.

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

   When a line's tokens land inside a command that is STILL WAITING (the
   classic: a script line after an `EXTRUDE h id` that parked at "Select
   profiles"), the refusal names it:
   `expected entity ids for command EXTRUDE (waiting at "Select profiles:")
   — put the ids on their own line, or a blank line before the next
   command`. A blank `.vks` line = Enter = finishes the pending command.

   And when the SAME line feeds two selection stages (`SWEEP 1 2`,
   `EXTRUDE 10 CUT 3`), the greedy gulp leaves the second stage starved, the
   command cancels itself, and — since issue #12 — headless mode refuses
   instead of returning a silent `ok:true` with an untouched document:
   `SWEEP cancelled itself with nothing done and no explanation (waiting at
   "Select path:")`. Give each stage its own `.vks` line.
6. **Optional stages hand foreign tokens back.** A command waiting at an
   optional keyword stage (`WORKPLANE XZ` at its `[OFFSET]` prompt)
   finishes on its default when the next token is not one of its keywords,
   and that token starts the NEXT command — `WORKPLANE XZ` on one `.vks`
   line directly followed by `RECT 0,0 10,10` runs both, no blank line
   needed. (Historic trap, fixed 2026-07-23: the first token of the next
   line was swallowed as the terminating keyword and the REST of that line
   silently dropped — an empty document with ok:true.)

7. **Unique prefixes resolve; ambiguous ones refuse.** Typed text that is a
   unique prefix of exactly one command runs it (`REC 0,0 10,10` = RECT —
   verified through `--exec`, `connect exec` and the GUI bar alike). An
   ambiguous prefix (`R ...`) runs NOTHING and the error lists the
   candidates (`ambiguous command R — matches RECT, REDO, REVOLVE, ROTATE,
   ROTATE3D`). Exact names and registered aliases always win over prefix
   matching (`EXT` is EXTRUDE's alias even though EXTEND also starts EX).
8. **RECT has a Dimensions mode** (AutoCAD RECTANG semantics): `D` (or
   `DIMENSIONS`) at either corner prompt asks `Length:` then `Height:`
   (signed — negative grows to the other side), then `First corner:` when
   none was given yet. All equivalent (verified):
   `RECT D 30 20 5,5`, `RECT 5,5 D 30 20`, and `RECT 5,5 35,25`.

Useful command vocabulary (aliases in parentheses): 2D drawing
`LINE (L) CIRCLE (C) ARC (A) RECT (REC) PLINE (PL) ELLIPSE (EL) SPLINE (SPL)`,
editing `MOVE (M) COPY (CO) ROTATE (RO) MIRROR (MI) SCALE (SC) ERASE (E)
TRIM (TR) OFFSET (O) FILLET (F) UNDO (U) REDO`, clipboard
`COPYCLIP <ids>` / `CUTCLIP <ids>` / `PASTECLIP [x,y]` (no point = paste at
the original position; the offline CLI uses a process-local buffer, the GUI
over IPC uses the REAL system clipboard — and PASTECLIP SELECTS what it
pasted, so clear with a bare `SELECT` before the next id-explicit command,
or pickfirst eats the typed ids), 3D
`WORKPLANE (WP) EXTRUDE (EXT) REVOLVE (REV) UNION SUBTRACT (SUB)
INTERSECT (INT) HOLE (HO) SHELL (SH) SPLIT COMBINE SWEEP (SW) LOFT (LO)
FILLET3D (F3D) CHAMFER3D (CH3D) MOVE3D (M3) ROTATE3D (RO3)
SETCOLOR TRANSPARENCY (TRANS)`, and the
index-addressed set in §3.

### 2b. Coloring solids headlessly: SETCOLOR

`SETCOLOR <ids> <#RRGGBB|name> [alpha]` styles entities for 3D output —
the colour and alpha land in the entity JSON (`color`/`transparency`),
which the .vkd persists, the 3D viewer honours, and the STEP writer turns
into STYLED_ITEM/COLOUR_RGB (the same path §"STEP couleurs" uses). alpha
is 0-100 percent TRANSPARENT (Enter = opaque, 2D entities have no alpha
channel and are ignored for it). Names: `black white red green blue
yellow cyan magenta gray/orange`. Pickfirst works: `SELECT 3` then bare
`SETCOLOR red 50`. A bad colour refuses with the accepted forms in the
message. Both channels (verified):

```sh
$CLI new --exec "RECT 0,0 40,30" --exec "EXTRUDE 10 1" \
         --exec "SETCOLOR 2 #E8AD23 30" --save-as colored.vkd
#   messages: [..., "1 entity(ies) recoloured (#e8ad23), 30% alpha"]
$CLI open colored.vkd --exec "SETCOLOR 2 red"      # named, alpha kept
$CLI open colored.vkd --exec "SELECT 2" --exec "SETCOLOR blue 50"  # pickfirst
$CLI open colored.vkd --exec "SETCOLOR 2 notacolor"
#   ok:true BUT messages: ["bad colour \"NOTACOLOR\" — use #RRGGBB ..."]
```

(TRANSPARENCY remains the selection-based sibling: `TRANS <pct> <ids>`.)

### 2a. Starting a blank 3D project: WORKPLANE plane → SKETCH → draw → EXTRUDE

`WORKPLANE XY|XZ|YZ [OFFSET d]` sets a **world-aligned** sketch plane — the
entry point of every from-scratch part. The frames are deterministic and
match the standard views (§5a): a sketch on XY reads upright in the TOP
view, XZ in FRONT, YZ in RIGHT. `OFFSET d` shifts the plane `d` mm along
its normal. Mapping of a sketch point `(a,b)` to world coordinates:

| plane | u axis | v axis | normal | `(a,b)` + OFFSET d → world |
|-------|--------|--------|--------|-----------------------------|
| `XY`  | +X     | +Y     | +Z     | `(a, b, d)`                 |
| `XZ`  | +X     | +Z     | −Y     | `(a, −d, b)`                |
| `YZ`  | +Y     | +Z     | +X     | `(d, a, b)`                 |

Back-compat: bare `WORKPLANE` = XY at Z=0; `WORKPLANE OFFSET d` = XY at
Z=d. EXTRUDE height runs along the plane **normal** (XZ grows toward −Y,
the FRONT viewer).

A 40×30 face profile on the front plane, extruded 25 mm (executed):

```sh
$CLI new --exec "WORKPLANE XZ" --exec "SKETCH NEW Front-1" \
         --exec "RECT 0,0 40,30" --exec "SKETCH CLOSE" \
         --exec "EXTRUDE 25 1" --exec "DESCRIBE" --save-as bracket.vkd
#   messages: ["work plane: XZ (front) at Y=0",
#     "sketch 'Front-1' opened (id 1) — ...",
#     "sketch 'Front-1' closed (work plane kept)",
#     "solid 2 created (height 25)",
#     "solid 2: volume=30000.0 mm3 area=5900.0 mm2
#      bbox=(0.0,-25.0,0.0)-(40.0,0.0,30.0) centroid=(20.0,-12.5,15.0)", ...]
# RECT u=0..40 -> X 0..40, v=0..30 -> Z 0..30, EXTRUDE 25 along -Y -> Y -25..0.
```

A d=20 disc on the right plane offset to X=40 (executed):

```sh
$CLI new --exec "WORKPLANE YZ OFFSET 40" --exec "SKETCH NEW Rib-1" \
         --exec "CIRCLE 20,15 10" --exec "SKETCH CLOSE" \
         --exec "EXTRUDE 5 1" --exec "DESCRIBE" --save-as rib.vkd
#   messages: ["work plane: YZ (right) at X=40", ...,
#     "solid 2: volume=1570.8 mm3 area=942.5 mm2
#      bbox=(40.0,10.0,5.0)-(45.0,30.0,25.0) centroid=(42.5,20.0,15.0)",
#     "sketch 1 'Rib-1': origin=(40.0,0.0,0.0) normal=(1.0,0.0,0.0) entities=1"]
# CIRCLE (20,15) r=10 -> centre Y=20 Z=15; EXTRUDE 5 along +X -> X 40..45.
```

Sketch names must be **one token** (the tokenizer splits on spaces). In the
GUI the same flow is Solids → New sketch → Top/Front/Right (or right-click
empty space in the 3D view); it auto-creates `PlaneSketch-N` through the
same commands.

In the GUI, right-clicking a **closed extrudable profile** in the 3D view
(sketch curve, circle, closed polyline — the same `wiresFromEntities`
criterion the EXTRUDE command applies) offers **Extrude…** before Move: a
height dialog in mm (remembers the last value), then the widget selects
the entity and submits `EXTRUDE <h>` through the shared CommandProcessor,
holding at the `Mode [New/Join/Cut/Symmetric] <New>` prompt with the live
ghost — Enter commits New. The headless parity line is plain
`EXTRUDE <h> <id>`; there is no GUI-only path.

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

   Trust `describe` (the LIVE field) over `query entities` for an
   entity's component name: `query entities` serializes the RAW entity
   JSON including the extra bag, so a script-built file can show a
   top-level `component` there that the entity itself never carried
   (issue #9 — the STEP writer reads the live field, and used to stamp
   `SOLID`/`SHELL`/`COMPOUND` auto-names as a result). Both levels are
   now accepted at load (top-level name adopted only when the canonical
   `geom.component` is empty), so a fresh `describe` after `open` is the
   verdict that matches what an export will write.

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
  Since issue #12, a command that cancels itself while saying NOTHING is a
  hard error in headless (`--exec`/`--run`) mode instead of a silent
  `ok:true`: the message names the command, the prompt it died at, and the
  cure (`.vks` script). `SWEEP 1 2` — two selection stages fed on one line
  — is the canonical case (§2 rule 5); the interactive GUI stays silent,
  because there a cancellation is the user's own Escape.
- **Building a TUBE (a round bar along a path): SWEEP, not a hand-rolled
  mesh.** `CIRCLE <centre> <radius>` for the section, then a `LINE`/`ARC`/
  `PLINE` for the spine, then a two-line `SWEEP` script (§2 rule 5). This is
  native and exact — no STL, no MESH2SOLID, no third-party mesh library.
  Verified, r=1.4 along a 50 mm line:

  ```sh
  printf 'SWEEP\n1\n2\n' > sw.vks     # 1 = profile (circle), 2 = path (line)
  $CLI new --exec "CIRCLE 0,0 1.4" --exec "LINE 0,0 0,50" \
           --run sw.vks --exec "DESCRIBE" --save-as tube.vkd
  #   "solid 3: volume=307.9 mm3 … bbox=(-1.4,0.0,-1.4)-(1.4,50.0,1.4)"
  #   pi*r^2*L = pi*1.4^2*50 = 307.88 mm3 — the section area times the length.
  ```

  A curved spine works the same way (`ARC cx,cy mx,my ex,ey` or a
  multi-segment `PLINE`), and the spine may sit on a `WORKPLANE`. A profile
  that does not touch the spine start still sweeps (the section is carried
  along the path). `SWEEP` accepts `LINE ARC PLINE` spines only — a `SPLINE`
  or a CLOSED polyline refuses by name ("no path geometry (need
  lines/arcs/polyline)", "path is not a single chain"). Sanity-check the
  result with `DESCRIBE`: for a tube, `volume ≈ pi*r^2*L` and the bbox is
  the spine's plus `r` on each side — a tube that comes back with a tiny
  volume and a huge bbox is a DISC, i.e. the section was read as the wrong
  thing.

  **A spine with SHARP CORNERS is supported, and now really keeps every
  leg** (issue #13). A multi-segment `PLINE` (the reporters' anatomical
  markers) used to come back with roughly HALF its material: before the
  fix, `0,0 → 0,25 → 25,25` with r=1.4 measured `volume=153.9` out of a
  nominal 307.9 (exactly 50 %), a 4-leg staircase 147.8 out of 295.6, and
  the shape was nonetheless reported `ok:true` as a normal success. The
  cause was OCCT's `BRepOffsetAPI_MakePipe`: over a spine whose direction
  breaks it builds a shape `BRepCheck` calls valid and whose shell is
  closed, holding only the FIRST leg. `SWEEP` now splits the spine at every
  direction break, sweeps one tangent-continuous leg at a time and fuses
  them — verified 304.2 (98.8 %) on the 2-leg corner, 284.6 (96.3 %) on a
  4 × 12 mm staircase and 454.5 (98.4 %) on a 3 × 25 mm one, while a
  straight or single-arc spine is untouched (307.9 / 100 %). Faithful sweeps
  of a corner fall a few percent short of `pi*r^2*L` because the elbow is
  trimmed, not filled; what matters is the order of magnitude:

  ```sh
  $CLI new --exec "CIRCLE 0,0 1.4" --exec "PLINE 0,0 0,25 25,25" \
           --run sw.vks --exec "DESCRIBE" --save-as elbow.vkd
  #   "solid 3: volume=304.2 mm3 … bbox=(-1.4,0.0,-1.4)-(26.4,25.0,1.4)"
  #   NOT 153.9 — a 50 % tube means the sweep stopped at the first corner.
  ```

  The old 50 % signature is still the diagnostic to keep in mind: if a
  cornered tube returns almost exactly half of `pi*r^2*L`, check that the
  `PLINE` really is a single chain (a disconnected spine refuses by name)
  and that every segment is `LINE`/`ARC`.
- **If you DO build meshes outside VikiCAD, watch the argument order.**
  The `manifold3d` Python wrapper is
  `Manifold.cylinder(height, radius_low, radius_high=-1, circular_segments=0,
  center=False)` — the FIRST argument is the HEIGHT (the Z extent), the
  second the radius. Writing the visually natural `cylinder(radius, length)`
  builds a huge flat disc instead of a tube: the reporter's r=1.4 × 85 mm
  groove tubes came out as 100 mm discs 1.4 mm thick, i.e. 30-60× oversized
  blobs, and only a numeric check caught it. Same family of trap in
  mesh helpers that take `(radius, height)`. This bites a hand-rolled
  generator, not VikiCAD — and inside VikiCAD the answer is the bullet
  above: SWEEP builds the tube directly, so there is nothing to export to
  Python in the first place.
- **`Manifold.rotate` does NOT align an axis — use `Manifold.transform`
  (issue #13).** The other half of the same mesh-generator story. The
  `manifold3d` wrapper's `rotate(v: Doublex3)` takes **Euler angles in
  degrees**, applied in `x-y-z` order ("*From the global reference frame, a
  model will be rotated in x-y-z order*" — its own docstring), NOT an
  axis-angle rotation. Handing it the natural Rodrigues vector
  `axis × angle_deg` therefore aligns the part only when the rotation
  happens to be a single-axis one; the general case drifts. Measured here
  (fit of the resulting cylinder's own axis, mesh covariance) on a
  `Manifold.cylinder(20, 1.4, 1.4, 32, center=True)`:

  | target direction | error with `rotate(axis*angle)` | error with `transform(3x4)` |
  |---|---|---|
  | pure YZ-plane polar angles (1…179°) | 0.0000° | 0.0000° |
  | `[1,-2,3]` normalized | 1.7801° | 0.0043° |
  | `[0.3,-0.7,0.65]` | 3.9045° | 0.0000° |
  | `[1,1,1]` normalized | 6.0905° | 0.0000° |
  | `[-2,1,1]` normalized | **6.7051°** | 0.0008° |

  A 6.7° misalignment on a 50 mm segment displaces the end disc sideways by
  `50·sin(6.7°) ≈ 5.8 mm` — half a radius and more, hence the "renflements
  aux coudes" (bulges at the elbows) and the ~150 % delivered volume the
  reporter measured. The cure is a manual orthonormal basis in a 3×4
  matrix, exactly as reported:

  ```py
  v = seg / np.linalg.norm(seg)                 # the direction to align onto
  seed = [0,0,1] if abs(v[2]) < 0.9 else [1,0,0]
  u = np.cross(seed, v); u /= np.linalg.norm(u)  # 1st basis vector
  w = np.cross(v, u)                             # 2nd (+Z maps to v)
  m = np.zeros((3, 4))
  m[:, 0], m[:, 1], m[:, 2] = u, w, v            # rotation
  m[:, 3] = midpoint                             # translation
  piece = Manifold.cylinder(dist, r, r, sections, center=True).transform(m)
  ```

  This is a trap in a THIRD-PARTY library, not in VikiCAD — but it is the
  trap that produced issue #13's report, and inside VikiCAD the answer
  stays the SWEEP bullet above. If you must orient one 3D part onto a
  direction (also true of OCCT's own `gp_Trsf::SetRotation(gp_Ax1, angle)`,
  which `ROTATE3D` uses), prefer building the basis and setting a
  `gp_Quaternion`/matrix over composing single-axis rotations.

- **DWG goes both ways, through LibreDWG.** Import: `$CLI import file.dwg
  --save-as out.vkd` (R14–2013 natively, 2018+ via the `dwg2dxf` fallback).
  Export: `$CLI export part.vkd out.dwg` (r2000) shells out to `dxf2dwg` —
  needs GNU LibreDWG on PATH or `~/.local/bin`, budget ~40 s per 5 000
  entities, and expect harmless `Unknown DXF code 21 for HATCH` warnings on
  some imported hatches. The failure verdict is the produced file, not the
  stderr noise. In the GUI, Save As offers DWG (and every other export):
  a non-VKD pick exports WITHOUT rebinding the working file — the open
  document stays the `.vkd`.
- **EAGLE 6+ opens natively, viewing-grade.** `$CLI import old.brd
  --save-as out.vkd` (or `.sch`; same dispatch in File>Open and the IPC
  `open` verb). Boards arrive FLATTENED — packages baked at their element
  positions on layers named and coloured like EAGLE's own table (mirrored
  elements swap to the b-layers), tracks are wide polylines, pads/vias
  drill-pierced solid hatches, pours their outline only (EAGLE never stores
  the computed fill, so neither do we). Schematics resolve gates/symbols,
  substitute `>NAME`/`>VALUE` and lay sheets SIDE BY SIDE with a 25 mm gap.
  There is no EAGLE export and no going back: this is a one-way viewing
  import, connectivity (contactrefs, classes, design rules) is not carried.
  A `.lbr` library opens as a CONTACT SHEET: one grid cell per package and
  symbol, names on a dedicated "Labels" layer, `>NAME`/`>VALUE` literal.
  Two refusals by design, each naming the cure in its message: EAGLE ≤5
  binaries (first byte 0x10 — resave as XML in EAGLE 6+) and `.sch` files
  from other EDA tools (content-sniffed, not extension).
- **IGES imports as surfaces, on purpose.** `$CLI import model.igs
  --save-as out.vkd` uses the same XCAF pipeline as STEP, but IGES files in
  the wild are trimmed surfaces: a solid-free file arrives as ONE viewable
  entity (view/measure/section fine; booleans need MESH2SOLID-style real
  geometry that IGES rarely carries). No IGES export.
- **Old P-CAD/Protel gerbers are first-class.** Aperture macros with `$n`
  variables/expressions bind at %ADD into concrete private macros
  (`NAME_Dnn`); `D01` with no prior `G01` assumes linear with a warning;
  `.top/.bot/.smt/.smb/.sst/.ssb/.spt/.spb` map to their kit roles. If a
  kit still skips a layer, read the `skipped` array — it names the file
  and the reason.
- **One routing for every open channel (issue #11).** The offline `open`
  verb, the IPC `open` method and the GUI's File>Open all go through the
  SAME dispatch: the file's extension picks the importer, the fallback for
  an unrecognised extension is the native store. Before #11 the CLI `open`
  was native-store-only (`open file.stl` died with *not a VikiCAD file (no
  entities table)* while `import file.stl` and the GUI took the very same
  file), which is a trap when an agent reaches for `open` out of habit.
  So `$CLI open file.stl --save-as out.vkd` and `$CLI import file.stl
  --save-as out.vkd` now produce the SAME document; `import` remains the
  explicit verb, `open` is the "just take this file" one. Read the reply's
  `dispatch` field when you need to know which arm ran. `--save` is
  REFUSED on an imported document (it would overwrite the SOURCE file with
  a `.vkd` payload) — name `--save-as OUT.vkd`.
- **An imported STL is a MESH, not a solid.** It arrives as one face carrying
  a triangulation and no surface — enough to view, measure, place
  (`MOVE3D`/`ROTATE3D`), section and re-export, but booleans, fillets and
  push/pull need real geometry and refuse with a message saying so.
  `MESH2SOLID <id>` sews the triangles into a BREP solid when you truly need
  that; it is capped at 20 000 triangles (`VIKICAD_MESH2SOLID_MAX` overrides)
  because sewing 50 k cost 39 s and 800 MB here, and the result is faceted
  either way. Both dialects are detected by content — a binary STL whose
  80-byte header happens to start with the word `solid` does not fool it.
  The sewn result never carries anti-material: mesh winding is arbitrary (a
  naive union of `trimesh` solids often exports inward-facing triangles), so
  every closed shell is checked and flipped outward when its signed volume
  comes out negative. A mesh may also hold SEVERAL bodies — a concatenated
  union sews into disjoint closed shells sharing no vertex — so the report
  counts them instead of calling the whole thing an "open shell", and one
  shape with 24 shells yields 24 solids:
  `solid 1: 5992 triangles sewn into 5992 faces (24 closed solids)`.
  A mesh with real holes keeps those as shells and still converts the rest
  (nothing is dropped), then the report reads e.g. `(2 closed solids, 1 open
  shell)`. The reply names the flip when one happened:
  `  1 shell(s) had inward-facing normals — reoriented so the volume comes
  out positive` — a positive volume is guaranteed, not silently fudged.
  **STEP export of a mesh loses the geometry silently** (the XCAF writer
  emits an EMPTY `MANIFOLD_SURFACE_SHAPE_REPRESENTATION` for a surface-less
  face) — which is why every STL import now warns: `import` returns a
  `warnings` array, the GUI history and `insertstep` print the same line
  ("...STEP export of a mesh writes no geometry — run MESH2SOLID <id>
  first"). Heed it before shipping a STEP of a downloaded part.
- **The unsaved-changes prompt is a MOUSE-only thing.** The GUI asks
  Save/Discard/Cancel on window close, New, Open and imports when the
  document is dirty — but the IPC `open` verb replaces the document with NO
  prompt (an agent manages its own saves; a modal would hang the socket),
  and stopping the systemd unit sends SIGTERM, which never runs the close
  handler. Save explicitly (`connect save PATH`) before `open` or unit stop
  if the state matters.
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
  `Mask`, `Silk`, `Paste`, `Outline`, `Drill`, `Drill-NPTH`, `Mech`;
  empty = none). Assigning a role RECOLORS the layer to the role palette
  and moves it to the role rank — the escape hatch when the outline
  election guessed wrong. `Drill-NPTH` (G3 closure) is how you say "the
  bare circles on this layer are UNPLATED holes": the Excellon writer and
  DRILLREPORT default untagged circles to non-plated there.

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
`scripts/gerber-export-diff.sh` replays that last claim on EVERY layer of
BOTH kits (31 rows, ~5 s, SKIP without gerbv/kits) and runs as the final
gui-smoke stage; it pairs files by SOURCE LAYER using the `fileLayers`
array the CLI `import` result now carries.

**G3-closure guardrails** (adversarial review of the export stage — every
behavior below is test-locked):

```sh
# NPTH is a real role now: a drawn mounting hole exports NON_PLATED.
$CLI open kitA.vkd --exec "LAYER Drill-NPTH CURRENT" \
     --exec "CIRCLE 50,-20 1.6" --save-as npth.vkd
$CLI export npth.vkd holes.TXT   # d=3.2 lands under ;TYPE=NON_PLATED
# Unknown export extension = hard error (it used to write DXF silently):
$CLI export kitA.vkd gerbers
#   {"error":{"code":"E_FORMAT","message":"unsupported export format..."}}
#   (a kit DIRECTORY must exist or end with '/')
# Role picks the dialect, the EXTENSION gets sanity-checked: exporting a
# Drill layer into thing.GBR (or copper into thing.txt) warns
#   "wrote an EXCELLON drill file, but '.gbr' implies RS-274X..."
# A hard mid-kit error removes the files already written and names them
# ("removed 9 partially written kit file(s): ...") — no partial kit.
# PANELIZE refuses grids over 2M cloned entities (typo guard):
#   "refusing to panelize: 100 x 100 cells x 2301 fab entity(ies) = ..."
```

Exported Gerber headers are silent for old consumers: X2 metadata rides in
Altium's comment form (`G04 #@! TF.GenerationSoftware,...*`), and a
regions-only layer (keepout zone) still defines ONE unused aperture so
pre-X2 sniffers (gerbv) recognize RS-274X instead of warning "RS-274D".

## 8. Animated kinematic chains headless: `anim render`

The animation module (`core/anim/`) drives a GENERIC kinematic chain —
typed joints `ball` / `revolute` (hinge) / `prismatic` (slider) / `fixed` /
`free` (root) — so the same command animates a humanoid, a robot arm or an
exploded assembly. Inputs are three JSON files (GenMov3D v1 schemas):
a *chain* (topology, rest pose, joint stops), a *pose3d* (sparse keyframes
of LOCAL rotations, carried forward when unspecified, slerp between keys,
loop block `pingpong|cycle|hold`, optional ignorable `breath`), and an
*avatar* (how the chain looks; `rigid` = parametric capsules).

```sh
vikicad-cli anim render --pose yog-vrksasana.json \
    --avatar manikin-neutral.json --out out/ \
    [--chain humanoid-12.json] [--fps 30] [--size 512x640] \
    [--formats glb,webp,png] [--camera side|front|three-quarter] \
    [--no-breath]
```

- Outputs in `--out`: `<pose-id>.glb` (Y-up metres, one animation, loop
  metadata in `extras`), `<pose-id>.webp` (transparent looping animation;
  `hold` plays once and freezes) and `<pose-id>-f0000.png`... when `png`
  is requested. Default formats: `glb,webp`.
- The chain file resolves next to the pose file, then `../chains/<chain
  id>.json`, unless `--chain` says otherwise. The avatar's `height_m`
  rescales the whole chain.
- Camera is FIXED for the whole clip (framed over the union of every
  frame, floor z=0 at the bottom); default `side` = eye on +X, the Y-Z
  silhouette. Rendering needs a reachable display (GLX); without one the
  command fails with a clear message — GLB export still works, it never
  renders.
- Joint values beyond the declared stops render AS AUTHORED and come back
  in `result.warnings`; nothing is clamped silently.
- Errors are the usual JSON on stdout AND a plain message on stderr (the
  GenMov3D pipeline flags on stderr).

Every run is deterministic: same inputs, byte-identical GLB and WebP.

The LIVE half is the Timeline dock (View > Timeline): load a pose3d +
avatar, play/scrub in the 3D view, shift a keyframe's time, adjust one
joint's channels at a keyframe, re-export the pose3d. Headless access via
`connect anim ...` (table in section 1b) — gui-smoke's timeline phase is
the reference script. Clips are capped at 300 s.

Avatar extension fields (tolerated by schema v1, proposed for v2 — the
full field list lives in the GenMov3D contract, entry
SCHEMA-AVATAR-V2-DEMANDE):

- `rigid.build`: `"capsule"` (default, historic look) or `"sculpted"` —
  revolved fusiform limbs with hemisphere end caps, elliptic torso loft
  with deltoid caps, pelvis blob, knuckle spheres; parameters under
  `rigid.sculpt` (per-joint `taper`/`flatten` maps, `bulge`, `torso`,
  `pelvis`). Mis-typed extension blocks REFUSE the file, never a silent
  capsule fallback.
- `presentation.ground_shadow` (0..1): translucent contact-shadow blob at
  z=0 under the animation's supports in rendered frames (never in the
  GLB). Adds partial-alpha pixels at the bottom of the frame.
- A chain may declare `accepts_pose_chains: ["<id>"...]`: pose3d files of
  those chains render on it with the extra joints at rest — the warning
  comes back in the CLI JSON and in the `anim load` IPC reply, and a
  re-export from the Timeline declares the chain it was EDITED on.
  `humanoid-14` (hands) accepts `humanoid-12` this way; pass the superset
  chain explicitly (`--chain` / third `anim load` argument).
