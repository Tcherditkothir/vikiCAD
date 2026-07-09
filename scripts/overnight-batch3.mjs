export const meta = {
  name: 'vikicad-overnight-fusion-b3',
  description: 'Overnight batch 3: OBJ export, standard views, persist workplane, annotation debt, HLR drawing derivation, auto-regions',
  phases: [
    { title: 'Implement' },
    { title: 'Sanity' },
  ],
}

// Third autonomous batch. Batches 1+2 landed 15 features (all green, 1113
// assertions). Same hardened rules: sequential, reset-at-start, idempotent,
// commit-only-if-green. Riskiest items (HLR drawing derivation, auto-region
// detection) are LAST so the safe wins commit first.

const REPO = '/home/lex/computer/vikicad'

const COMMON = `
You are working in the VikiCAD C++/Qt6/OpenCASCADE repo at ${REPO}.
READ FIRST (do not skip): docs/FUSION_GAP.md (roadmap), docs/REPRISE.md
(handoff + how to build/run/test), docs/LESSONS.md (known pitfalls). Follow the
architecture: geometry in mm; solids are SolidEntity wrapping TopoDS_Shape;
solid ops in core/solid/SolidOps.{h,cpp}; STL export already exists in core/io
(exportStl) — mirror its meshing (BRepMesh_IncrementalMesh) for OBJ. 2D entities
live in core/doc (LineEntity, ArcEntity, PolylineEntity, TextEntity,
DimensionEntity with a DimStyle). Commands are state machines in
core/cmd/Commands*.cpp with NUMERIC PARAMS BEFORE entity selection. Every
mutation goes through a document transaction (undo is automatic). The document
work plane is documentWorkplane(doc) (WorkPlane = {gp_Pnt origin; gp_Dir normal;
gp_Dir xDir}); .vkd persistence is core/io/NativeStore.{h,cpp} with a meta table.

RULES (strict):
0. IDEMPOTENCY. This feature may already exist from a previous run. FIRST grep
   core/ for the key symbol named in the feature. If it already exists AND the
   full suite is green, do NOT re-implement — report committed=false, summary
   "already present". Otherwise implement it.
1. START CLEAN. Before touching anything, run:
     cd ${REPO} && git reset --hard HEAD && git clean -fd core tests gui cli docs scripts
   This drops leftover junk from a prior agent that died mid-run (all COMMITTED
   work is safe in HEAD). Do NOT abort on a dirty tree. Never touch build/.
2. Implement the feature in core/ (testable) + a command and/or CLI verb if it
   helps. Match existing code style.
3. Add a Catch2 test in tests/ that proves it. Register any NEW test file in
   tests/CMakeLists.txt. OCCT pitfall: BRepPrimAPI/algo Make*::IsDone() is
   unreliable before the shape is built — force .Shape()/.Build() and null-check.
4. Build: cmake --build build/debug -j$(nproc). Fix all errors.
5. Run the FULL suite: ./build/debug/tests/vikicad-tests. It MUST be all-green.
6. If green: git add -A && git commit with a clear message ending with the
   Co-Authored-By trailer for Claude. If NOT green after real effort: run
   "git reset --hard HEAD" then "git clean -fd core tests gui cli", and report
   committed=false. LEAVE THE TREE CLEAN either way.
7. NEVER git push. NEVER touch build config beyond adding source/test files.
8. Do NOT start the GUI (it gets reaped); verify via tests + CLI only.
Return a one-paragraph report: what you built, the test you added, committed=true/false.
`

const FEATURES = [
  {
    name: 'export-obj',
    prompt: `Add Wavefront OBJ mesh export for 3D printing / interchange, right
next to the existing STL export. core/io: exportObj(doc, path, deflection)
meshing every SolidEntity with BRepMesh_IncrementalMesh and writing an ASCII OBJ
(v / f lines; optionally vn normals). Add CLI dispatch so
"vikicad-cli export FILE.vkd OUT.obj [--deflection MM]" works (mirror the .stl
branch in cli/main.cpp). Test: a 10mm box exports an OBJ that parses back to 8
unique vertices (or >=8) and 12 triangular faces; assert the file is non-empty
and starts with OBJ content. Keep it self-contained (no new third-party dep).`,
  },
  {
    name: 'standard-views-core',
    prompt: `Add the CORE of ViewCube / standard views (the widget itself is GUI,
but the projection math is testable). Add a small core helper (e.g.
core/render or core/solid) standardViewDir(name) returning the view direction +
up vector for Top/Bottom/Front/Back/Left/Right/Iso, and alignToFaceDir(face) ->
(dir, up) that looks along a planar face's normal. Expose an IPC/CLI-free but
unit-testable API. Test: standardViewDir("TOP") looks down -Z with +Y up;
"FRONT" looks along -Y (or +Y) with +Z up; alignToFaceDir on a box's +Z face
returns a -Z view direction. (If you also add a GUI verb, keep it optional.)`,
  },
  {
    name: 'persist-workplane',
    prompt: `Close the FUSION_GAP debt: persist the current work plane in the .vkd
and clear stale sketch-on-face reference snaps when leaving a sketch. Store
documentWorkplane(doc) (origin, normal, xDir) in the NativeStore meta table on
save and restore it on load. Test: set a non-default work plane (e.g. WORKPLANE
OFFSET 100, or an explicit origin/normal), save to a temp .vkd, load it back,
and assert the restored work plane matches (origin/normal/xDir within 1e-9).`,
  },
  {
    name: 'annotation-debt',
    prompt: `Two small 2D annotation debts from FUSION_GAP §6. (a) MTEXT
word-wrap: TextEntity gains an optional column width; when set, buildPrimitives
wraps words to that width using the font metrics already used for text layout.
(b) DIMSTYLE dimpost: a suffix/prefix string (DXF DIMPOST, code 3) appended to
dimension text (e.g. " mm"), imported from DXF and applied in the dimension's
regenerated text. Tests: a long MTEXT with a narrow column width produces
multiple text lines (line count > 1); a linear dimension with dimpost "mm"
renders text ending in "mm".`,
  },
  {
    name: 'drawing-derivation-hlr',
    prompt: `HIGH VALUE (the user is a draftsman): derive a 2D drawing from a 3D
solid via Hidden Line Removal. core/solid or core/render: projectToDrawing(
shape, gp_Dir viewDir) using HLRBRep_Algo + HLRBRep_HLRToShape, returning the
VISIBLE edges (and optionally hidden edges separately) as a set of 2D
polylines/segments in the view plane. Add a MAKEVIEW / DRAWINGVIEW command that
projects a picked solid to the current 2D space as line/arc entities (a flat
"mise en plan" of the chosen standard view). Test: projecting a 10x10x10 box
along -Z (top view) yields a closed 10x10 square outline (assert the projected
2D bounding box is ~10x10 and there are >=4 visible edges).`,
  },
  {
    name: 'auto-regions',
    prompt: `Fusion-style auto-region detection: from a set of intersecting 2D
curves, find the closed regions they bound (instead of requiring one pre-closed
loop). Add core/geom (or core/solid) findRegions(entities) that computes curve
intersections, splits curves at crossings, and extracts the minimal closed
faces (a planar arrangement / face-finding). Start with lines + circles + arcs;
robustness over completeness. If a full planar-arrangement is too big for one
sitting, implement a REAL but minimal version that passes the test and commit
that foundation. Test: two overlapping circles yield 3 regions (2 lunes + 1
lens), or at minimum the lens (intersection) region is found; a plus-shaped set
of 4 overlapping rectangles yields the expected closed regions. Keep the test
modest and deterministic.`,
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
const sanity = await agent(
  `${COMMON}\n\nDo NOT implement anything. Just: cd ${REPO}, run a clean build
(cmake --build build/debug -j$(nproc)) and the full test suite
(./build/debug/tests/vikicad-tests). Report the exact final "All tests passed"
line (or the failures). If the tree is dirty/uncommitted, run
"git reset --hard HEAD" to clean it and note that. Return committed=false always;
summary = the test line.`,
  { label: 'final-sanity', phase: 'Sanity', schema: REPORT, effort: 'medium' },
)

const done = results.filter((r) => r && r.committed).map((r) => r.feature)
log(`Batch 3 done. Committed: ${done.length}/${FEATURES.length} -> ${done.join(', ')}`)
return { committed: done, sanity }
