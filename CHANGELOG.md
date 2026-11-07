# Changelog

## Unreleased

- **Issue #12: a command that cancels itself while saying NOTHING is now an
  error in headless mode, and the tube the reporter needed is built natively
  by SWEEP.** The report was about the `manifold3d` Python wrapper taking
  `cylinder(height, radius_low, ...)` — a hand-written tube generator built
  100 mm discs instead of r=1.4 tubes. VikiCAD has no manifold3d dependency;
  the trap was upstream. But reproducing the need natively exposed a real
  VikiCAD trap of its own: `SWEEP 1 2` (profile and path on ONE line) fed
  both ids to the greedy `EntitySet` gulp, starved the path stage, and the
  command cancelled itself — the CLI answered `{"ok":true}` with an
  untouched document and zero messages, indistinguishable from success.
  `EXTRUDE 10 CUT 3` (target then profiles) had the same silence. Fix:
  `drive()` records where a command gave up (name + prompt) and
  `submit()` in STRICT mode turns an unexplained cancellation into a named
  error naming the cure (a `.vks` script, AGENT.md §2 rule 5). Two guards:
  the interactive GUI stays silent (an Escape IS the user's answer) and a
  command that already explained itself (LOFT names its refusal) is left
  untouched. Docs: AGENT.md §2 rule 5 + §7 gotchas — the manifold3d
  argument-order warning the issue asked for, AND the native answer:
  `CIRCLE` section + `LINE`/`ARC`/`PLINE` spine + a two-line `.vks` SWEEP
  builds the tube exactly (verified volume = pi·r²·L, 307.9 mm3 for
  r=1.4 × 50 mm; 773.8 mm3 across a curved 125.66 mm arc). Tests:
  `tests/test_silent_cancel.cpp` (6 cases, tag `[issue12]`, 29 assertions);
  gui-smoke `silent cancel:` + `tube:` phases.

- **Issue #9: a solid whose component name lives at the TOP level of its
  .vkd JSON (as scripts assembling documents by SQL write it) no longer
  loses it.** The canonical home is `geom.component` — what the STEP
  writer reads to name PRODUCT and what `DESCRIBE` shows. A script-built
  file with the name top-level only used to load an UNNAMED solid: the
  STEP export then stamped the XCAF auto-names (`SOLID`/`SHELL`/
  `COMPOUND`) instead of the real part names. Fix is a read-side
  fallback, not a writer change: `Entity::fromJson` gains a
  `postFromJson(obj)` second pass (virtual, no-op by default) and
  `SolidEntity` adopts the top-level `component` ONLY when
  `geom.component` came back empty — the canonical field wins when both
  levels carry a name, and the next save converges the file (the name
  migrates into `geom.component`). Real-world check on the 112-solid
  PSY1014 v4 document: before = 27 named PRODUCTs + 85 generic; after =
  112 PRODUCTs, 112 unique names, 0 generic. Tests:
  `tests/test_solid_component.cpp` (5 cases: load survival, canonical
  precedence, anonymity, .vkd round-trip with a SQL-style hot-patch,
  STEP PRODUCT naming). ctest 464/464.

- **Issue #11: the CLI `open` verb now dispatches by extension, exactly like
  the GUI's File>Open.** `$CLI open file.stl --save-as out.vkd` used to fail
  with `not a VikiCAD file (no entities table): file.stl` while
  `$CLI import file.stl --save-as out.vkd` and the GUI accepted the very same
  file — three copies of one policy, only two of them knowing the imported
  formats. `open` now goes through a single shared dispatch
  (`core/io/FileOpen.{h,cpp}`, `openFileAny()`): directory = Gerber kit,
  `.vkd` native, `.dxf`/`.dwg`, `.step`/`.stp`, `.igs`/`.iges`, `.stl`,
  EAGLE `.brd`/`.sch`/`.lbr`, a lone Gerber/Excellon file by content sniff,
  and the native store as the fallback so a renamed or extensionless `.vkd`
  keeps opening. Component tagging (file stem where STEP/IGES carried no part
  name, always for STL) is identical to File>Open. The `open` reply gains a
  `dispatch` field naming the arm taken AND mirrors the `import` verb's
  counters (`solids` `triangles` `imported` `skipped` `files` `entities`
  `layers` `warnings`), so the two verbs read alike. New guard: `open f.stl
  --save` is refused with `E_ARGS` (a bare `--save` would overwrite the
  SOURCE file with `.vkd` bytes) and the message names `--save-as`;
  `--save` on a natively-opened `.vkd` is unchanged. Docs: §1a/§1b rows now
  name the shared routing, plus a §7 gotcha explaining `open` vs `import`.
  Tests: `tests/test_file_open.cpp` (7 cases, tag `[issue11]`); gui-smoke
  `open dispatch:` phase. ctest 464/464.

- **Issue #10: `connect` reply wait window is tunable
  (`--timeout MS` / `VIKICAD_CONNECT_TIMEOUT`).** The CLI is a
  synchronous RPC client and the GUI answers only once the whole method
  finished — the fixed 10 s silence window made heavy `connect open`
  calls (>100 MB .vkd: 112 OCC solids took ~1-5 min) fail with a fake
  `E_TIMEOUT` while the load was progressing fine (the model WAS mounted
  — the client just gave up early). The window is now a per-call flag
  (`connect open big.vkd --timeout 900000`, positioned anywhere on the
  line) and an env var (`VIKICAD_CONNECT_TIMEOUT`, wins over the flag);
  bad values and unknown `--` options refuse with `E_ARGS`; the
  E_TIMEOUT message names both levers. The window is a silence budget
  re-armed by every reply chunk, so a large `query entities` flowing in
  chunks is never killed mid-transfer while a wedged GUI still aborts.
  Doc: AGENT.md §1c. gui-smoke: `cli timeout:` argument-handling phase.

- **Issue #8 — MESH2SOLID: signed volume normalised, and every shell of a
  concatenated mesh kept.** A mesh whose triangles wind INWARD sewed into
  a watertight solid whose volume came out NEGATIVE (measured −1000 mm³
  on a 10 mm cube, −353 on the reporter's tubes), so every downstream
  consumer saw anti-material. A concatenated union (two overlapping
  bodies sharing no vertex — the naive trimesh export) sewed into a
  COMPOUND of disjoint closed shells, which the single-shell code path
  reported as an "open shell" and never made a solid of. Both are fixed
  per shell: a closed shell becomes a solid, a solid measuring negative
  is reoriented outward (`BRepLib::OrientClosedSolid`, with an explicit
  shell reversal as a fallback), and everything — solids plus any
  genuinely open shell — is collected into one compound, nothing
  dropped. The MESH2SOLID report now gives the body count ("2 closed
  solids") instead of a bare "open shell", and names the flip when one
  happened. Measured: 28/28 real trimesh meshes of the SNC model set now
  convert to a positive volume — `tronc-cerebral.stl` was 19 104.76 mm³
  (14 of its 24 shells subtractive) and is 21 465.58 mm³ (its real
  material); `arachnoide` 2 579 743.52 → 2 579 782.83. 4 new regression
  test cases in tests/test_stl_import.cpp (inside-out cube, two disjoint
  shells, mixed winding, message wording).

- **Issues #1-#4 from the headless drawing agent (SNC session).** One
  batch, four fixes: (1) the one-invocation-one-document CLI contract is
  spelled out (AGENT.md §1a) and `SKETCH CLOSE` with no sketch now
  explains the fresh-document cause instead of a bare "no sketch is
  open"; (2) the empty-gulp EntitySet refusal names the command still
  waiting and its prompt ("expected entity ids for command EXTRUDE
  (waiting at \"Select profiles:\")..."), `--exec` accepts `;`-separated
  complete commands, and E_EXEC messages carry the failing segment; (3)
  new `SETCOLOR <ids> <#RRGGBB|name> [alpha]` command (hex or basic
  names, alpha = percent transparent, pickfirst, one-transaction undo)
  feeding the existing per-entity colour/transparency path — .vkd,
  viewer and STEP STYLED_ITEM/COLOUR_RGB all honoured (AGENT.md §2b,
  tests/test_setcolor.cpp); (4) STL import returns a `warnings` array
  (CLI JSON + GUI history) telling the caller a mesh cannot cross STEP
  (empty MANIFOLD_SURFACE_SHAPE_REPRESENTATION) and naming MESH2SOLID
  as the cure — no implicit conversion, the explicit choice stays the
  rule. ctest 448/448; gui-smoke 333/334 (the single failure is a stale
  GUI holding the IPC socket, not the code).

- **Sculpted avatar build, studio lighting, hands (aesthetic worksite).**
  The rigid manikin gains a `build: "sculpted"` mode (schema-v1-tolerated
  extension fields, proposed for v2): limbs are single revolved fusiform
  solids closed by hemisphere caps (tapering to the child joint's radius
  so bent knees/elbows stay continuous at any angle), the torso is an
  elliptic loft (smoothed cubic profile, dense ruled sections — shoulders
  melt into deltoid caps, marked waist, sculpted pelvis blob on the
  root), knuckle spheres bridge every articulation, and per-joint
  taper/flatten maps shape feet and hands. The offscreen renderer moves
  to per-pixel shading with a camera-relative three-point light rig,
  material derived from the avatar's roughness/metallic, and an optional
  translucent contact shadow anchored under the animation's supports
  (`presentation.ground_shadow`). A new `humanoid-14` chain adds ~9 cm
  wrist-articulated mitten hands; its `accepts_pose_chains` extension
  lets the existing humanoid-12 pose bank render unchanged with hands at
  rest (explicit warning, surfaced in the CLI JSON and the `anim load`
  IPC reply). Contract CLI unchanged; byte determinism kept; ~2.4 s per
  240-frame pose; heaviest GLB 1.24 MB. Three selectable avatar variants
  shipped to GenMov3D with a 4x5 comparison sheet; the sage `sculpte`
  variant won a three-lens judge panel, and an adversarial review
  (6 finders + refuters) confirmed 6 defects, all fixed (bounded torso
  loft on short generic joints, direction-agnostic deltoids, strict
  extension parsing with no silent fallbacks, edit-chain re-export).
- **Timeline dock (GUI half of the animation module).** Load a pose3d +
  avatar (avatar selector from the sibling directory), play/scrub the
  clip on the 3D view through a new non-pickable animation overlay posed
  per frame via `SetLocalTransformation` (never a scene rebuild —
  scrubbing back to a frame is pixel-EXACT), clickable keyframe markers,
  light editing (shift a keyframe's time, adjust one joint's channels)
  and pose3d re-export. Fully agent-drivable over IPC (`connect anim
  load|frame|play|stop|clear|status`); gui-smoke gained a timeline phase
  (334 checks green).
- **Animation module: generic kinematic chains, animated GLB, looping
  WebP (`vikicad-cli anim render`).** New `core/anim/`: typed joints
  (ball/revolute/prismatic/fixed/free), sparse keyframes densified with
  carry-forward, quaternion slerp, forward kinematics, per-channel joint
  stops that warn without clamping. Avatars dress a chain through the
  `AvatarProvider` seam (rigid = fused OCCT capsules + ellipsoid head; a
  skinned provider can plug in later). Exports an animated binary glTF
  (hand-rolled writer, validated in tests by an independent vendored
  reader, cgltf 1.15) and an animated transparent WebP (libwebpmux,
  streaming encoder), both byte-deterministic. Offscreen rendering lives
  in a new `vikioffscreen` lib (virtual window + RGBA dump, background at
  alpha 0) so visualization stays out of vikicore. Proven on the 15 real
  GenMov3D pilot poses: 15/15 render, 240 frames each in ~4.5 s, zero
  warnings, WebP 0.4-1.2 MB per loop.
- **Aperture macros with variables and arithmetic (RS-274X).** `%AM` bodies
  using `$n` variables, assignments and expressions (`+ - x /`, parentheses,
  unary minus) now parse: the macro is bound at `%ADD` time into a private,
  fully numeric copy, so rings, the inspector, camMeta and the RS-274X
  writer never see a variable. This is what P-CAD/Protel kits (the classic
  `OC8` octagon) are made of. Two more old-writer archaisms accepted with a
  warning instead of a refusal: `D01` before any `G01/G02/G03` (linear has
  been the de-facto default since RS-274D) and parameters passed to a
  variable-free macro. Protel short extensions (`.top/.bot`, `.smt/.smb`,
  `.sst/.ssb`, `.spt/.spb`) get their proper kit roles. A 7-file P-CAD kit
  that previously imported 1 file now imports 7 (39 130 entities).
- **IGES import (.igs/.iges).** Same XCAF pipeline as STEP
  (`IGESCAFControl_Reader`), so colours and names survive where the file
  carries them. IGES models are usually trimmed surfaces, not solids — a
  solid-free file imports as ONE viewable entity instead of failing.
  Import only; VikiCAD does not write IGES.
- **EAGLE libraries (.lbr) open as a contact sheet.** Every package and
  symbol becomes one cell of a grid, its name printed underneath on a
  dedicated "Labels" layer; `>NAME`/`>VALUE` stay literal exactly like
  EAGLE's own library editor. Proven against all 73 XML libraries in the
  vault (up to 495 679 entities) with zero skipped nodes.
- **EAGLE 6+ import (.brd/.sch), viewing-grade.** File ▸ Open, File ▸ Import
  EAGLE, the IPC `open` verb and `vikicad-cli import` all read EAGLE XML
  boards and schematics natively (QXmlStreamReader, no new dependency).
  Boards are flattened: packages at their element positions (bottom-side
  elements mirror AND swap to the b-layers), copper tracks become polylines
  with their real widths and round caps, pads/vias solid annuli pierced by
  their drill, pours their honest outline (EAGLE never stores the computed
  fill). Schematics resolve part → deviceset → gate → symbol, substitute
  `>NAME`/`>VALUE` (smashed attributes included), draw pins with names,
  nets with junction dots and net-name labels, and lay multiple sheets side
  by side. EAGLE layers become document layers with EAGLE's own names,
  16-colour palette, visibility and a paint order that stacks like EAGLE
  (bottom copper first, annotations last). EAGLE dimensions map to live
  DimensionEntity/LeaderEntity. Proven against the whole vault: 145/145
  real files (67 boards, 78 schematics up to 5 sheets) import with ZERO
  skipped nodes. Deliberate refusals name the cure: v5-and-older binaries
  ("open in EAGLE 6+ and save"), .lbr libraries, and non-EAGLE `.sch` files
  from other tools. Closing the window, New, Open and the imports
  now ask Save / Discard / Cancel when the drawing has unsaved changes, and
  the title bar marks the state with the classic `*`. Tracking is an id
  comparison on the undo journal (each committed transaction gets a
  lifetime-unique state id), so undoing back to the last saved state clears
  the flag, and a state abandoned on a dead redo branch can never pass for
  saved. Imported documents (DXF/DWG/STEP/STL/Gerber kits) start life
  modified — they exist nowhere as a `.vkd` yet. Headless flows are
  untouched: the IPC `open`/`save` verbs never prompt.
- **DWG export.** `File ▸ Export ▸ DWG`, the Save As dialog, the IPC
  `export` verb and `vikicad-cli export FILE.vkd OUT.dwg` all write DWG
  r2000 through GNU LibreDWG's `dxf2dwg` (PATH or `~/.local/bin`, the same
  lookup as the import fallback). The intermediate DXF is made canonical
  first — two vendored libdxfrw patches: `0005` gives every entity its
  owner handle (330) and drops the empty CLASSES section (without owners,
  LibreDWG stored the entities as orphans and every reader saw an EMPTY
  drawing), `0006` writes the MTEXT rotation as the X-axis direction
  vector 11/21/31 (group 50 is fatal to LibreDWG, and readers guessing
  its unit no longer have to). Proven at scale: a 5 186-entity imported
  drawing exports and reads back 5 186/5 186 with all types intact,
  block-embedded hatches included. Conversion runs in a temp dir and the
  target is only written on success.
- **Save As speaks every format.** The Save dialog now offers VKD, STEP,
  DXF, DWG, STL and OBJ. Picking a non-native format EXPORTS (same engine
  as File ▸ Export) and leaves the open document untouched — path, title
  and dirty flag stay on the `.vkd`; a lossy format never silently becomes
  the working file. With no typed suffix, the selected filter decides.
- **Clipboard: Ctrl+C/X/V, including across documents.** New `COPYCLIP`,
  `CUTCLIP` and `PASTECLIP` commands (Edit menu, standard shortcuts) put a
  self-contained JSON payload on the system clipboard under the
  `application/x-vikicad-entities` MIME type: entities travel with the
  layers, block definitions and dimension styles they reference — and
  solids with their BREP — so pasting works in the same document, in
  another document, or in another running VikiCAD. Layers are matched by
  name (created when missing; an existing layer of the same name wins).
  Paste asks for an insertion point with a live ghost (2D and 3D); Enter
  pastes at the original coordinates, the cross-document alignment case.
  The pasted set becomes the selection. Typing in the command bar still
  copies/pastes text — focused text fields keep the standard keys.
- **STL import**: `.stl` files open as a mesh — one OCCT face carrying the
  triangulation, so viewing, measuring, sectioning and placing
  (`MOVE3D`/`ROTATE3D`, insert-as-component) all work. Both dialects are
  detected by content, which matters because a binary STL's 80-byte header
  often begins with the word `solid`. Booleans and fillets need real
  geometry and refuse with a message saying so; `MESH2SOLID` (alias `M2S`)
  sews the triangles into a BREP solid on demand, capped at 20 000
  triangles (`VIKICAD_MESH2SOLID_MAX` overrides) because the cost is
  dominated by memory — 52 k triangles measured at 39 s and 817 MB.
- **STEP colour, transparency and part names**, both directions. Import
  now goes through `STEPCAFControl_Reader`/XCAF and looks colour up per
  SOLID (real exporters style each body, not the part), export through
  `STEPCAFControl_Writer`. A ByLayer solid is deliberately left unstyled.
  New `colored`/`named` counts report what the FILE carried, so "no
  colour" is never mistaken for "colour lost".
- **OBJ export writes materials**: a `.mtl` beside the `.obj`, one material
  per colour+transparency pair, plus `o <component>` names. A model with no
  explicit colour gets no `.mtl` and no `mtllib` line.
- Fixed: OCCT printed parse diagnostics on **stdout**, which corrupted the
  CLI's JSON reply on a malformed STL. Silencing is now shared by every
  importer (`core/io/OcctMessages.h`).

## 0.2.0 — 2026-07-17

Fusion-style 3D interaction, full headless/agent parity, and a new PCB
fabrication (Gerber/Excellon) editor.

- **3D interaction**: the 3D view is an input device — hovering a face sets
  the work plane and drives the ghost preview (red = material removed, blue
  = added); box-select drag, right-drag orbit / short right-click = a
  tree-structured context menu (Hole ▸/Face ▸/Edges ▸/Move ▸/Select ▸);
  Alt+click / "Select ▸" opens a candidate resolver that highlights each
  option, including an X-ray ghost that glows *through* occluding solids;
  ViewCube; SPLIT/COMBINE solids by a plane or curved face; a parametric
  `FeatureTree` (hole/shell/extrude…) editable from the Properties panel.
- **Sketches v1**: lightweight, named, drawn on any face or work plane,
  visible in the 3D view, with no dependency from an already-generated
  solid back onto its source sketch (editing a used-up sketch never
  regenerates the part).
- **Multi-STEP assemblies**: `ASSEMBLY` command, assembly tree panel,
  per-solid color/transparency, and multi-file selection when inserting STEP
  components.
- **Agent parity**: every mouse action has a headless equivalent —
  index-addressed `INSPECT`/`FEATEDIT`/`PUSHPULL`/`SHELLOPEN`/`SPLITFACE`/
  `FILLETEDGES`/`CHAMFEREDGES`/`MATE`/`DRAFT`, `DESCRIBE` + `query --describe`
  (computed volume/area/bbox/centroid/features, no BREP blob), IPC `viewdir`
  for a view→screenshot→diff loop. The complete guide,
  **[docs/AGENT.md](docs/AGENT.md)**, was validated end-to-end by an agent
  given nothing but the guide itself.
- **PCB fabrication editor (new)**: read, inspect, measure, edit and
  re-export **Gerber RS-274X and Excellon** files — without a full EDA suite.
  Open a whole fab-output kit (directory or single file, GUI/CLI/IPC) with
  one layer per file, correct negative (LPC) polarity rendering, a CAM-style
  layer stack (per-layer transparency/paint order/role, mirrored bottom
  view), edge-to-edge clearance measurement (`MINDIST`), an aperture
  inspector, drill reports, and an RS-274X/Excellon writer whose output was
  checked pixel-for-pixel against `gerbv` (the reference renderer) on real
  fabrication kits. `PANELIZE`, and a DXF↔Gerber bridge (draw a board
  outline in 2D, export it as a clean `.GKO`). See
  **[docs/PCB_CAM.md](docs/PCB_CAM.md)**.
- File > Export (STEP/DXF/STL/OBJ/Gerber kit) and the matching IPC `export`
  verb.
- Hardening: previously-dead Ctrl+Z/Ctrl+Y (ambiguous Qt shortcuts), booleans
  that silently produced zero solids, leaked-transaction undo corruption —
  all fixed at the root with regression tests.
- Test suite: 1506 → **5142 assertions across 334 cases**; the live-GUI
  regression harness (`scripts/gui-smoke.sh`): 0 → **224 checks**, including
  an automated visual diff of Gerber output against `gerbv`.

## 0.1.0 — 2026-07-09

First public release.

- **2D drafting**: LINE/CIRCLE/ARC/RECT/PLINE/ELLIPSE/SPLINE/POINT/XLINE;
  MOVE/COPY/ROTATE/MIRROR/SCALE/STRETCH; TRIM/EXTEND/OFFSET/FILLET/CHAMFER/
  BREAK/JOIN/EXPLODE; object snaps, ORTHO/POLAR/GRID, vertex grips, layers,
  mm/inch toggle.
- **Annotation**: MTEXT, five dimension types regenerated live from DimStyle,
  leaders, hatches, MATCHPROP.
- **Organization**: blocks with attributes, associative rect/polar arrays,
  sticky notes (markdown, pinnable to entities), layouts with exact-scale PDF
  plotting.
- **Interop**: DXF R12–2018 import/export (vendored libdxfrw), DWG import via
  external `dwg2dxf`, STEP round-trip with sidecar notes.
- **3D solids**: work planes, EXTRUDE/REVOLVE from closed 2D profiles,
  UNION/SUBTRACT/INTERSECT, FILLET3D/CHAMFER3D, push/pull on faces, shaded
  OCCT view with interactive 3D selection, parametric feature tree.
- **Assembly**: multi-STEP component tree with per-solid color/transparency.
- **Export**: STL/OBJ mesh export, PDF plots, DXF, STEP.
- **Automation**: headless `vikicad-cli` with single-line JSON output, `.vks`
  command scripts (AutoCAD `.scr` semantics), JSON-RPC IPC socket on the
  running GUI (`vikicad-cli connect`).
- Native SQLite `.vkd` file format with full undo journal.
