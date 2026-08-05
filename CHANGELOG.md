# Changelog

## Unreleased

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
