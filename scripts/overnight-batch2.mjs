export const meta = {
  name: 'vikicad-overnight-fusion-b2',
  description: 'Overnight batch 2: 3D patterns, sweep/loft, interference, snap modes, draft, section, feature-history foundations',
  phases: [
    { title: 'Implement' },
    { title: 'Sanity' },
  ],
}

// Second autonomous batch (batch 1 landed 8 features: extrude-modes, shell,
// fillet/chamfer-edges, mate, STL, measure3d, sketch-ref-snap, params — plus a
// manual HOLE). Same rules as batch 1: sequential, reset-at-start, idempotent,
// commit-only-if-green. Riskiest item (parametric feature history) is LAST so
// everything before it is already committed if it destabilises.

const REPO = '/home/lex/computer/vikicad'

const COMMON = `
You are working in the VikiCAD C++/Qt6/OpenCASCADE repo at ${REPO}.
READ FIRST (do not skip): docs/FUSION_GAP.md (roadmap), docs/REPRISE.md
(handoff + how to build/run/test), docs/LESSONS.md (known pitfalls). Follow the
architecture: geometry in mm; solids are SolidEntity wrapping TopoDS_Shape
(SolidEntity::shape(), applyTrsf(gp_Trsf), fields component + transparency);
solid ops live in core/solid/SolidOps.{h,cpp} (extrudeWires, revolveWires,
booleanOp, makeHole, shellSolid, filletEdges, chamferEdges, mateTransform,
minDistance, pushPullFace, planeFromFace, faceOutline2d; WorkPlane =
{gp_Pnt origin; gp_Dir normal; gp_Dir xDir}; documentWorkplane(doc)). Commands
are state machines in core/cmd/Commands*.cpp with NUMERIC PARAMS BEFORE entity
selection (the greedy EntitySet swallows numbers otherwise). Every mutation goes
through a document transaction (undo is automatic). Assembly transforms: see
core/cmd/CommandsAssembly.cpp (placement() -> gp_Trsf, applied to each solid).

RULES (strict):
0. IDEMPOTENCY. This feature may already exist from a previous run. FIRST grep
   core/ for the key symbol named in the feature. If it already exists AND the
   full suite is green, do NOT re-implement — report committed=false, summary
   "already present". Otherwise implement it.
1. START CLEAN. Before touching anything, run:
     cd ${REPO} && git reset --hard HEAD && git clean -fd core tests gui cli docs scripts
   This drops leftover junk from a prior agent that died mid-run (all COMMITTED
   work is safe in HEAD). Do NOT abort on a dirty tree. Never touch build/.
2. Implement the feature in core/ (testable) + a command and/or IPC verb if it
   helps. Match existing code style.
3. Add a Catch2 test in tests/ that proves it (volumes via BRepGProp, bounds via
   Bnd_Box, distances via BRepExtrema_DistShapeShape, etc.). Register any NEW
   test file in tests/CMakeLists.txt. OCCT pitfall: BRepPrimAPI_Make*::IsDone()
   is unreliable before the shape is built — force .Shape() and null-check.
4. Build: cmake --build build/debug -j$(nproc). Fix all errors and warnings you
   introduce.
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
    name: 'interference-detection',
    prompt: `Add assembly interference detection. solidops::interferenceVolume(
a, b) -> double: the volume of BRepAlgoAPI_Common(a, b) (0 when disjoint or only
touching). Add an INTERFERE command (headless-testable) that takes two solid ids
and reports the overlap volume; and/or a core sweep checkAllInterferences(doc)
returning the pairs that overlap. Test: two 10^3 boxes overlapping by a 10x10x2
slab report interference volume ~200; two disjoint boxes report ~0.`,
  },
  {
    name: 'pattern-3d',
    prompt: `Add 3D patterns of a solid body (Fusion "Rectangular/Circular
Pattern"). solidops has the box; the placement math mirrors CommandsAssembly's
gp_Trsf. Add PATTERN3D commands: RECT (counts nx,ny,nz + spacings dx,dy,dz) and
POLAR (count + axis X/Y/Z + total angle, about a center) that CLONE the picked
solid into N transformed copies (each a new SolidEntity, sharing component). Keep
numeric params before the solid pick. Test: a rectangular 3x2x1 pattern of a box
with spacing 20 yields 6 solids whose combined bounding box spans the expected
extent; a polar pattern of 4 about Z yields 4 solids.`,
  },
  {
    name: 'sweep-loft',
    prompt: `Add SWEEP and LOFT. solidops::sweepProfile(profileWires, pathWire)
using BRepOffsetAPI_MakePipe, and loftProfiles(vector<wires> sections, bool
solid) using BRepOffsetAPI_ThruSections. Add SWEEP (pick profile entities then a
path entity) and LOFT (pick 2+ profile entities in order) commands via
wiresFromEntities + documentWorkplane. Tests: sweeping a radius-2 circle along a
straight 50mm line gives volume ~= pi*2^2*50; lofting two concentric squares
(10x10 at z0 and 6x6 at z20 — build via workplane offset) yields a positive
volume between the two extremes.`,
  },
  {
    name: 'snap-modes-extra',
    prompt: `Extend the SnapEngine with the missing modes noted in FUSION_GAP:
NEAREST (closest point on an entity to the cursor), NODE (a Point entity), and
TANGENT (tangent-from-last-point to a circle/arc). Wire them into the snap
priority and the snap type enum; keep them toggleable like the existing modes.
Test in tests/test_snap.cpp: NEAREST on a line returns the foot of perpendicular;
NODE snaps to a POINT entity; TANGENT from an external point to a circle returns
a point whose radius-to-tangent is perpendicular to the tangent line (dot ~0).`,
  },
  {
    name: 'draft-taper',
    prompt: `Add DRAFT (angled taper of faces, for moldability). solidops::
draftFaces(solid, vector<TopoDS_Shape> faces, gp_Dir pullDir, gp_Pln neutralPlane,
double angleDeg) using BRepOffsetAPI_DraftAngle. Since face picking is GUI, ALSO
provide a headless-testable core entry that drafts the side faces of a box given
the pull direction and neutral plane. Test: drafting the 4 side faces of a
10x10x10 box by a few degrees (pull +Z, neutral plane at z=0) changes the volume
away from 1000 but keeps it positive and within a sane band (e.g. 700..1300).`,
  },
  {
    name: 'section-analysis',
    prompt: `Add a SECTION core op: cut a solid by a plane and return the section
profile + its area. solidops::sectionArea(solid, gp_Pln) -> double using
BRepAlgoAPI_Section (build the section wires, face them, measure area with
BRepGProp::SurfaceProperties), and sectionWires(solid, gp_Pln) returning the
cut curves. Add a SECTION command (headless: solid id + a plane keyword XY/XZ/YZ
at an offset) reporting the area. Test: sectioning a 10^3 box with the plane z=5
gives a 10x10 section of area ~100.`,
  },
  {
    name: 'feature-history-foundations',
    prompt: `Lay the FOUNDATIONS of a parametric feature history (the core of
Fusion) WITHOUT ripping out the current SolidEntity/commands — purely additive,
in new files core/solid/FeatureTree.{h,cpp}. Model a FeatureNode variant for at
least: Sketch (a set of profile entity descriptions or wire params) and Extrude
(height, mode, target). A FeatureTree owns an ordered list of nodes and can
regenerate() a TopoDS_Shape by replaying them (Sketch -> wires -> extrudeWires).
Editing a node's parameter (e.g. the extrude height) and re-running regenerate()
must produce the updated shape. Do NOT change how existing solids are stored yet;
this is the groundwork a later milestone will wire into SolidEntity/.vkd. Add a
Catch2 test tests/test_feature_tree.cpp: build a tree {sketch rect 20x20, extrude
h=10} -> volume 4000; set the extrude height to 25 -> regenerate -> volume 10000.
Register the new sources in core/CMakeLists.txt and the test in tests/CMakeLists.txt.
If it gets too invasive, keep the data model + replay minimal but REAL (must pass
the regenerate test) and commit that foundation.`,
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
log(`Batch 2 done. Committed: ${done.length}/${FEATURES.length} -> ${done.join(', ')}`)
return { committed: done, sanity }
