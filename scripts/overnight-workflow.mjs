export const meta = {
  name: 'vikicad-overnight-fusion',
  description: 'Autonomously implement + test VikiCAD Fusion-gap features overnight',
  phases: [
    { title: 'Implement' },
    { title: 'Sanity' },
  ],
}

// Runs AFTER compaction while Lex sleeps (~5-6h). Each feature is implemented
// by one agent IN SEQUENCE (never in parallel — they share the repo working
// tree). Every agent must: build, add a Catch2 test, run the FULL suite, and
// commit ONLY if green; otherwise `git checkout -- .` to leave the tree clean
// for the next agent. Sequential + revert-on-fail = no broken tree, no
// conflicts. Order = highest value × headless-testability first (see
// docs/FUSION_GAP.md).

const REPO = '/home/lex/computer/vikicad'

const COMMON = `
You are working in the VikiCAD C++/Qt6/OpenCASCADE repo at ${REPO}.
READ FIRST (do not skip): docs/FUSION_GAP.md (roadmap), docs/REPRISE.md
(handoff + how to build/run/test), docs/LESSONS.md (known pitfalls). Follow the
architecture: geometry in mm; solids are SolidEntity wrapping TopoDS_Shape;
solid ops live in core/solid/SolidOps.{h,cpp}; commands are state machines in
core/cmd/Commands*.cpp with NUMERIC PARAMS BEFORE entity selection; every
mutation goes through a document transaction (undo is automatic).

RULES (strict):
0. IDEMPOTENCY. This feature may already be implemented by a previous run.
   FIRST grep core/ for the key symbol named in the feature (e.g. the function
   or command name). If it already exists AND the full suite is green, do NOT
   re-implement — report committed=false, summary "already present". Otherwise
   implement it.
1. START CLEAN. Before you touch anything, run:
     cd ${REPO} && git reset --hard HEAD && git clean -fd core tests gui cli docs scripts
   This discards leftover junk from a prior agent that died mid-run (all
   COMMITTED work is safe in HEAD; only uncommitted changes are dropped). Do
   NOT abort on a dirty tree — clean it and proceed. Never touch build/.
2. Implement the feature in core/ (testable) + a command and/or IPC verb if it
   helps. Keep it consistent with existing code style.
3. Add a Catch2 test in tests/ that proves it (volumes via GProp, bounds via
   Bnd_Box, etc.). Register new test files / sources in the CMakeLists.
   OCCT pitfall: BRepPrimAPI_Make*::IsDone() is unreliable before the shape is
   built — force .Shape() and null-check instead of trusting IsDone().
4. Build: cmake --build build/debug -j$(nproc). Fix all errors.
5. Run the FULL suite: ./build/debug/tests/vikicad-tests. It MUST be all-green.
6. If green: git add -A && git commit with a clear message ending with
   the Co-Authored-By trailer for Claude. If NOT green after real effort:
   run "git reset --hard HEAD" then "git clean -fd core tests gui cli", and
   report committed=false. LEAVE THE TREE CLEAN either way.
7. NEVER git push. NEVER touch build config beyond adding source/test files.
8. Do NOT start the GUI (it gets reaped); verify via tests + CLI only.
Return a one-paragraph report: what you built, the test you added, and
committed=true/false.
`

// NOTE: hole-feature was implemented + committed manually (866e93d) after the
// first overnight run died on API 529 overload — it is intentionally NOT in
// this list. The idempotency rule (0) makes re-runs safe regardless.
const FEATURES = [
  {
    name: 'extrude-modes',
    prompt: `Extend EXTRUDE with modes: symmetric (both sides), and cut vs join
vs new-body against an existing solid. Add solidops::extrudeWires overloads or
a mode enum {NewBody, Join, Cut, Symmetric}. The EXTRUDE command gains a
keyword step [New/Join/Cut/Symmetric] <New>. When Join/Cut, ask which solid to
combine with (BRepAlgoAPI Fuse/Cut). Tests: symmetric doubles the height about
the plane; cut removes the prism volume from a target box.`,
  },
  {
    name: 'shell',
    prompt: `Add SHELL (hollow a solid to a wall thickness, optionally opening
one face). solidops::shellSolid(solid, thickness, optional openFace) using
BRepOffsetAPI_MakeThickSolid. Add a SHELL command (thickness, then pick solid;
optional pick a face to open via pick3d/pickedFace flow is GUI — for the
command, shell all-around, i.e. no open face, is enough for v1). Test: shelling
a 10x10x10 box by 1mm leaves volume = 1000 - 8*8*8 approx (hollow) — assert the
shelled volume is clearly less than solid and > 0.`,
  },
  {
    name: 'fillet-selected-edges',
    prompt: `Allow FILLET3D / CHAMFER3D on SPECIFIC edges, not only all edges.
Add solidops::filletEdges(solid, vector<TopoDS_Shape> edges, radius) and
chamferEdges(...). Since edge picking is GUI, ALSO add a headless-testable core
function that fillets the first N edges of a shape (by TopExp order) so a test
can drive it. Test: filleting all 12 edges of a 10^3 box by r=1 reduces volume
by a known small amount (assert 0 < newVol < 1000).`,
  },
  {
    name: 'assembly-mate-faces',
    prompt: `Add an assembly MATE constraint. solidops::mateTransform(
faceA_on_movingSolid, faceB_on_fixedSolid) -> gp_Trsf that moves the first
solid so faceA becomes coincident with and anti-parallel to faceB (planar
faces). Apply via SolidEntity::applyTrsf. Add a MATE command later; for now a
core function + test. Test: two boxes; mate the +Z face of one onto the +Z face
of the other; after applying the trsf, the two faces are coplanar (distance ~0
via BRepExtrema_DistShapeShape) and normals opposed.`,
  },
  {
    name: 'export-stl',
    prompt: `Add STL export for 3D printing. core/io: exportStl(doc, path,
deflection) meshing every SolidEntity with BRepMesh_IncrementalMesh and writing
binary or ASCII STL (StlAPI_Writer is fine). Add CLI: vikicad-cli export
FILE.vkd OUT.stl. Test: a 10^3 box exports an STL that parses back to 12
triangles (or a positive triangle count); assert the file is non-empty and has
the STL header.`,
  },
  {
    name: 'measure-3d',
    prompt: `Add 3D measurement core: solidops::minDistance(shapeA, shapeB) ->
double using BRepExtrema_DistShapeShape, and a MEASURE3D that (headless) takes
two solid ids and reports the min distance. Test: two boxes 5mm apart along X
report min distance 5.`,
  },
  {
    name: 'sketch-ref-snap',
    prompt: `Make the sketch-on-face reference outline SNAPPABLE. When a sketch
reference is active (MainWindow sets it), feed its vertices + arc/circle
centers into the snap engine as endpoint/center targets so the user can snap
the profile to the real face features. Prefer a clean core hook: e.g. the
CommandContext or Document holds optional "extra snap points" that SnapEngine
also considers. Test: with extra snap points set, snapQuery near one returns
it.`,
  },
  {
    name: 'user-parameters',
    prompt: `Add a user-parameter table: named values with expressions (d = 10,
w = 2*d). A small expression evaluator (+ - * / parentheses, references to
other params). Store in Document, persist in .vkd. Add PARAM name expr command.
This is groundwork for driving dimensions/features later. Test: set d=10, w=2*d
-> w evaluates to 20; changing d re-evaluates w.`,
  },
]

phase('Implement')

const REPORT = {
  type: 'object', additionalProperties: false,
  required: ['feature', 'committed', 'summary'],
  properties: {
    feature: { type: 'string' },
    committed: { type: 'boolean' },
    summary: { type: 'string' },
  },
}

const results = []
for (let i = 0; i < FEATURES.length; i++) {
  const f = FEATURES[i]
  log(`(${i + 1}/${FEATURES.length}) implementing ${f.name}`)
  const r = await agent(`${COMMON}\n\nFEATURE: ${f.name}\n${f.prompt}`, {
    label: f.name,
    phase: 'Implement',
    schema: REPORT,
    effort: 'high',
  })
  results.push(r || { feature: f.name, committed: false, summary: 'agent died' })
  log(`${f.name}: committed=${r ? r.committed : false}`)
}

phase('Sanity')
// Final guard: make sure the tree still builds & the whole suite is green.
const sanity = await agent(
  `${COMMON}\n\nDo NOT implement anything. Just: cd ${REPO}, run a clean build
(cmake --build build/debug -j$(nproc)) and the full test suite
(./build/debug/tests/vikicad-tests). Report the exact final "All tests passed"
line (or the failures). If the tree is dirty/uncommitted, run git stash to set
it aside and note that. Return committed=false always; summary = the test line.`,
  { label: 'final-sanity', phase: 'Sanity', schema: REPORT, effort: 'medium' },
)

const done = results.filter((r) => r && r.committed).map((r) => r.feature)
log(`Overnight done. Committed: ${done.length}/${FEATURES.length} -> ${done.join(', ')}`)
return { committed: done, sanity }
