#!/usr/bin/env bash
# gui-smoke.sh — LIVE-GUI regression harness for VikiCAD.
#
# Drives the REAL GUI (started as a systemd user unit) through the IPC
# socket and asserts at every step, combining `connect query` entity
# counts with screenshot image-hash comparison (python3 + PIL).
#
# Scenario:
#   2D : RECT, CIRCLE, MOVE (+bounds), UNDO/REDO (counts change and restore)
#   3D : EXTRUDE, view3d on, screenshot A ; HOLE -> screenshot B != A ;
#        UNDO -> == A ; REDO -> == B ; LIST -> solid metrics (volume/area/
#        bbox/features) ; viewdir ISO/TOP -> renders differ (camera moved) ;
#        SPLIT XY -> 2 solids ; COMBINE -> 1 ;
#        export STL -> non-empty + header/size check.
#   Gerber kit (real S5M0PCBA, SKIPs when absent): IPC open of the fab
#        directory -> layer list, stable non-empty render, one UNDO
#        empties the document, REDO restores it (screenshot diffs);
#        G2 layer stack: BOARDVIEW TOP (bottom side dimmed) / BOTTOM
#        (top side dimmed + X-mirrored view) / ALL (identical to the
#        initial clean render), LAYER <name> ALPHA over IPC;
#        G2 measuring: MINDIST drill-to-drill (JSON trailer vs the by-hand
#        formula, witness overlay on the canvas), DIMALIGNED pad-center to
#        pad-center (stable clean capture, UNDO restores the render).
#
# Prints a PASS/FAIL table; exits non-zero on any FAIL. ALWAYS stops the
# vikicad-gui unit at the end (trap), even on error/interrupt.
#
# Run this before handing anything to Lex. Dependencies: bash, python3-PIL.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLI="$ROOT/build/debug/cli/vikicad-cli"
GUI="$ROOT/build/debug/gui/vikicad"
UNIT=vikicad-gui
TMP="$(mktemp -d /tmp/vikicad-smoke.XXXXXX)"

# Image comparison thresholds. "Same" renders are checked with a 32x32
# dhash (Hamming distance out of 1024 bits, measured 0 for identical GL
# dumps); "different" renders with the percentage of pixels that changed
# by more than 16 gray levels, in basis points (measured ~300+ bp for the
# 12 mm demo hole, 0 for identical renders). The wide gaps absorb GL
# rasterization jitter without flaking.
SAME_HASH_MAX=8    # dhash bits
DIFF_PIXELS_MIN=50 # basis points = 0.50 % of pixels

FAILS=0
ROWS=()

cleanup() {
    systemctl --user stop "$UNIT" 2>/dev/null
    systemctl --user reset-failed "$UNIT" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

# ---- tiny assertion kit ---------------------------------------------------

record() { # status name detail
    ROWS+=("$1|$2|$3")
    [[ "$1" == FAIL ]] && FAILS=$((FAILS + 1))
}

assert_eq() { # name expected actual
    if [[ "$2" == "$3" ]]; then record PASS "$1" "= $2"
    else record FAIL "$1" "expected '$2', got '$3'"; fi
}

assert_ne() { # name not-expected actual
    if [[ "$2" != "$3" ]]; then record PASS "$1" "'$3' != '$2'"
    else record FAIL "$1" "value unexpectedly stayed '$2'"; fi
}

assert_le() { # name value max
    if [[ "$2" -le "$3" ]]; then record PASS "$1" "$2 <= $3"
    else record FAIL "$1" "$2 > $3"; fi
}

assert_ge() { # name value min
    if [[ "$2" -ge "$3" ]]; then record PASS "$1" "$2 >= $3"
    else record FAIL "$1" "$2 < $3"; fi
}

print_table() {
    echo
    printf '%-4s | %-38s | %s\n' RES STEP DETAIL
    printf -- '-----+----------------------------------------+------------------------------\n'
    local row
    for row in "${ROWS[@]}"; do
        IFS='|' read -r st name detail <<<"$row"
        printf '%-4s | %-38s | %s\n' "$st" "$name" "$detail"
    done
    echo
    if [[ "$FAILS" -eq 0 ]]; then echo "gui-smoke: ALL GREEN (${#ROWS[@]} checks)"
    else echo "gui-smoke: $FAILS FAILED out of ${#ROWS[@]} checks"; fi
}

die() { # hard abort (harness cannot even run the scenario)
    record FAIL "$1" "$2"
    print_table
    exit 1
}

# ---- JSON / IPC helpers ---------------------------------------------------

jget() { # json python-expression-over-d  (prints the value)
    printf '%s' "$1" | python3 -c \
        "import json,sys; d=json.load(sys.stdin); print($2)" 2>/dev/null
}

rpc() { "$CLI" connect "$@" 2>/dev/null; }

gexec() { # name command...  -> asserts the command was accepted
    local name="$1"; shift
    local out ok
    out="$(rpc exec "$@")"
    ok="$(jget "$out" "d.get('ok')")"
    if [[ "$ok" == "True" ]]; then record PASS "$name" "exec ok"
    else record FAIL "$name" "exec failed: $out"; fi
}

count() { jget "$(rpc query entities)" "d['result']['count']"; }

bounds() { jget "$(rpc query bounds)" "d['result']['bounds']"; }

solid_ids() { # space-separated ids of solid entities, ascending
    jget "$(rpc query entities)" \
        "' '.join(str(e['id']) for e in sorted(d['result']['entities'], key=lambda e: e['id']) if e['type']=='solid')"
}

max_id() {
    jget "$(rpc query entities)" \
        "max(e['id'] for e in d['result']['entities'])"
}

shot() { # name path  -> screenshot + non-empty assertion
    local name="$1" path="$2"
    local out ok
    out="$(rpc screenshot "$path")"
    ok="$(jget "$out" "d.get('ok')")"
    if [[ "$ok" == "True" && -s "$path" ]]; then
        record PASS "$name" "$(stat -c%s "$path") bytes"
    else
        record FAIL "$name" "screenshot failed: $out"
    fi
}

img_cmp() { # imageA imageB -> "hash_dist pixel_diff_bp"
    python3 - "$1" "$2" <<'PYEOF'
import sys
import warnings
warnings.simplefilter("ignore")  # PIL getdata deprecation noise
from PIL import Image, ImageChops

def gray(path):
    return Image.open(path).convert("L")

def dhash(img, size=32):
    small = img.resize((size + 1, size), Image.LANCZOS)
    px = list(small.getdata())
    bits = []
    for row in range(size):
        line = px[row * (size + 1):(row + 1) * (size + 1)]
        bits.extend(1 if line[c] < line[c + 1] else 0 for c in range(size))
    return bits

a, b = gray(sys.argv[1]), gray(sys.argv[2])
ha, hb = dhash(a), dhash(b)
hash_dist = sum(x != y for x, y in zip(ha, hb))
hist = ImageChops.difference(a, b).histogram()
changed = sum(hist[17:])  # pixels that moved by > 16 gray levels
total = a.size[0] * a.size[1]
print(hash_dist, changed * 10000 // max(total, 1), changed)
PYEOF
}

img_hash_dist() { img_cmp "$1" "$2" | cut -d' ' -f1; }
img_pixel_bp() { img_cmp "$1" "$2" | cut -d' ' -f2; }
# Raw changed-pixel count — for SMALL deterministic 2D deltas (a dimension,
# a witness line) that basis points would floor away.
img_px_changed() { img_cmp "$1" "$2" | cut -d' ' -f3; }

# ---- (1) build if needed --------------------------------------------------

echo "gui-smoke: building (no-op when up to date)..."
if ! cmake --build "$ROOT/build/debug" --parallel "$(nproc)" >"$TMP/build.log" 2>&1; then
    tail -30 "$TMP/build.log"
    die build "build failed (see above)"
fi
[[ -x "$CLI" && -x "$GUI" ]] || die build "missing $CLI or $GUI"
record PASS build "binaries up to date"

# ---- (2) start the GUI as a systemd user unit -----------------------------

systemctl --user stop "$UNIT" 2>/dev/null
systemctl --user reset-failed "$UNIT" 2>/dev/null
systemd-run --user --unit="$UNIT" --collect \
    --setenv=QT_QPA_PLATFORM=xcb \
    ${DISPLAY:+--setenv=DISPLAY="$DISPLAY"} \
    "$GUI" >/dev/null 2>&1 || die gui-start "systemd-run failed"

# ---- (3) wait for the IPC socket to answer ping ---------------------------

up=""
for _ in $(seq 1 50); do # ~10 s
    if [[ "$(jget "$(rpc ping)" "d['result'].get('pong')")" == "True" ]]; then
        up=1; break
    fi
    sleep 0.2
done
[[ -n "$up" ]] || die gui-ping "GUI did not answer ping within 10 s"
record PASS gui-ping "IPC socket answering"

# ---- (4) scenario ----------------------------------------------------------

# --- UI layout phase --------------------------------------------------------
# The tool tab strip (Draw..Views) and the tabified right-side docks
# (Layers/Properties/Assembly as tabs, Properties in front) self-describe
# through `query ui`.
ui_json="$(rpc query ui)"
assert_eq "ui: tool tab strip present" \
    "Draw,Modify,Annotate,Measure,Blocks,Solids,Views" \
    "$(jget "$ui_json" "','.join(d['result']['toolTabs'])")"
assert_eq "ui: right docks tabified" \
    "Assembly,Layers,Properties" \
    "$(jget "$ui_json" "','.join(sorted(d['result']['tabbedDocks']))")"

# --- 2D phase ---------------------------------------------------------------
assert_eq "2d: fresh document empty" 0 "$(count)"

# viewdir needs the 3D view: refused while the 2D canvas is active.
out="$(rpc viewdir TOP)"
assert_eq "2d: viewdir refused outside 3D" False "$(jget "$out" "d.get('ok')")"

gexec "2d: WORKPLANE XY" "WORKPLANE XY"

gexec "2d: RECT" "RECT 0,0 40,30"
assert_eq "2d: count after RECT" 1 "$(count)"
rect_id="$(max_id)"

gexec "2d: CIRCLE" "CIRCLE 100,15 10"
assert_eq "2d: count after CIRCLE" 2 "$(count)"
circle_id="$(max_id)"

bounds_before_move="$(bounds)"
gexec "2d: MOVE circle +50,0" "MOVE $circle_id 0,0 50,0"
assert_eq "2d: count unchanged by MOVE" 2 "$(count)"
assert_ne "2d: bounds changed by MOVE" "$bounds_before_move" "$(bounds)"

gexec "2d: UNDO move" "UNDO"
assert_eq "2d: bounds restored by UNDO" "$bounds_before_move" "$(bounds)"

gexec "2d: UNDO circle" "UNDO"
assert_eq "2d: count after UNDO circle" 1 "$(count)"

gexec "2d: REDO circle" "REDO"
assert_eq "2d: count after REDO circle" 2 "$(count)"

gexec "2d: UNDO back to rect only" "UNDO"
assert_eq "2d: count back to 1" 1 "$(count)"

# --- 3D phase ---------------------------------------------------------------
gexec "3d: EXTRUDE rect h=10" "EXTRUDE 10 $rect_id"
assert_eq "3d: one entity after EXTRUDE" 1 "$(count)"
solid_id="$(solid_ids)"
assert_ne "3d: a solid exists" "" "$solid_id"

# INSPECT: the sub-shape discovery verb agents use to learn face/edge
# indices (deterministic TopExp order) before PUSHPULL/FILLET-style calls.
out="$(rpc exec "INSPECT $solid_id All")"
assert_eq "inspect: box has 6 faces" 6 "$(jget "$out" \
    "sum(1 for m in d['result']['messages'] if m.startswith('face '))")"
assert_eq "inspect: box has 12 edges" 12 "$(jget "$out" \
    "sum(1 for m in d['result']['messages'] if m.startswith('edge '))")"
assert_eq "inspect: face 0 is a plane" True "$(jget "$out" \
    "any(m.startswith('face 0: plane area=') for m in d['result']['messages'])")"
out="$(rpc exec "INS $solid_id Edges")"
assert_eq "inspect: Edges scope lists no faces" 0 "$(jget "$out" \
    "sum(1 for m in d['result']['messages'] if m.startswith('face '))")"

out="$(rpc view3d on)"
assert_eq "3d: view3d on" True "$(jget "$out" "d['result'].get('is3d')")"

shot "3d: screenshot A (box)" "$TMP/a.png"

gexec "3d: HOLE d=12 through" "HOLE 12 T 20,15 $solid_id"
shot "3d: screenshot B (hole)" "$TMP/b.png"
assert_ge "3d: B differs from A (hole visible)" \
    "$(img_pixel_bp "$TMP/a.png" "$TMP/b.png")" "$DIFF_PIXELS_MIN"

gexec "3d: UNDO hole" "UNDO"
shot "3d: screenshot A' (undo)" "$TMP/a2.png"
assert_le "3d: A' == A (undo restores render)" \
    "$(img_hash_dist "$TMP/a.png" "$TMP/a2.png")" "$SAME_HASH_MAX"

gexec "3d: REDO hole" "REDO"
shot "3d: screenshot B' (redo)" "$TMP/b2.png"
assert_le "3d: B' == B (redo restores render)" \
    "$(img_hash_dist "$TMP/b.png" "$TMP/b2.png")" "$SAME_HASH_MAX"

# FEATEDIT: the headless twin of the Properties panel. LIST discovers the
# bore parameters; an edit regenerates the solid (render changes); one UNDO
# restores it; a param/node mismatch reports cleanly over IPC.
solid_id="$(solid_ids)" # id may have changed across undo/redo
out="$(rpc exec "FEATEDIT LIST $solid_id")"
assert_eq "featedit: LIST shows bore diameter" True "$(jget "$out" \
    "any(m == 'hole 1: diameter = 12.0' for m in d['result']['messages'])")"
assert_eq "featedit: LIST shows bore centre" True "$(jget "$out" \
    "any(m == 'hole 1: center x = 20.0' for m in d['result']['messages'])")"

gexec "featedit: widen bore to d=16" "FEATEDIT diameter 16 1 $solid_id"
shot "featedit: screenshot C (wider bore)" "$TMP/c.png"
assert_ge "featedit: C differs from B (bore widened)" \
    "$(img_pixel_bp "$TMP/b2.png" "$TMP/c.png")" "$DIFF_PIXELS_MIN"

gexec "featedit: UNDO edit" "UNDO"
shot "featedit: screenshot B'' (undo)" "$TMP/b3.png"
assert_le "featedit: B'' == B (undo restores bore)" \
    "$(img_hash_dist "$TMP/b.png" "$TMP/b3.png")" "$SAME_HASH_MAX"

out="$(rpc exec "FEATEDIT thickness 2 1 $solid_id")"
assert_eq "featedit: kind mismatch reports" True "$(jget "$out" \
    "any('no editable' in m for m in d['result']['messages'])")"

# DESCRIBE + query describe: the agent's "understand the model" view, both
# renderings. The live solid is the 40x30x10 box with the d=12 through-hole
# at (20,15) -> volume = 12000 - pi*36*10, and the feature line/JSON must
# carry the bore parameters. The machine view must never leak brep blobs.
out="$(rpc exec "DESCRIBE")"
assert_eq "describe: document line" True "$(jget "$out" \
    "any(m.startswith('document: units=mm entities=') for m in d['result']['messages'])")"
assert_eq "describe: solid volume line" True "$(jget "$out" \
    "any(m.startswith('solid $solid_id: volume=10869.0 mm3') for m in d['result']['messages'])")"
assert_eq "describe: hole feature line" True "$(jget "$out" \
    "'  hole 1 d=12 through @(20,15)' in d['result']['messages']")"
desc_json="$(rpc query describe)"
assert_eq "describe: query volume numeric" True "$(jget "$desc_json" \
    "abs(d['result']['describe']['solids'][0]['volume'] - (12000 - 3.141592653589793*36*10)) < 1e-3")"
assert_eq "describe: query hole diameter" True "$(jget "$desc_json" \
    "d['result']['describe']['solids'][0]['features'][0]['diameter'] == 12")"
assert_eq "describe: no brep key anywhere" True "$(jget "$desc_json" \
    "'brep' not in json.dumps(d['result']['describe'])")"

# LIST on the smoke solid: the quick-numbers line (shared metrics helper
# with DESCRIBE). Box 40x30x10 minus the d=12 bore -> volume=10869.0,
# feature history = base + hole = 2 nodes.
out="$(rpc exec "LIST 20,0")"
assert_eq "list: solid metrics line" True "$(jget "$out" \
    "any('volume=10869.0 mm3' in m and 'bbox=(0.0,0.0,0.0)-(40.0,30.0,10.0)' in m and 'features=2' in m for m in d['result']['messages'])")"

# --- viewdir phase: the agent's literal eyes ---------------------------------
# viewdir aims the camera along a standard view (and FitAlls); an image-hash
# distance between the ISO and TOP renders proves the camera really moved.
out="$(rpc viewdir ISO)"
assert_eq "viewdir: ISO accepted" True "$(jget "$out" "d['result'].get('ok')")"
shot "viewdir: screenshot ISO" "$TMP/v_iso.png"
out="$(rpc viewdir TOP)"
assert_eq "viewdir: TOP accepted" True "$(jget "$out" "d['result'].get('ok')")"
shot "viewdir: screenshot TOP" "$TMP/v_top.png"
assert_ge "viewdir: TOP differs from ISO (camera moved)" \
    "$(img_hash_dist "$TMP/v_iso.png" "$TMP/v_top.png")" "$((SAME_HASH_MAX + 1))"
out="$(rpc viewdir SIDEWAYS)"
assert_eq "viewdir: unknown view refused" False "$(jget "$out" "d.get('ok')")"

solid_id="$(solid_ids)"
gexec "3d: SPLIT XY z=5" "SPLIT XY 5 $solid_id"
assert_eq "3d: two solids after SPLIT" 2 "$(count)"

read -r piece_a piece_b <<<"$(solid_ids)"
gexec "3d: COMBINE pieces" "COMBINE $piece_a $piece_b"
assert_eq "3d: one solid after COMBINE" 1 "$(count)"

# --- sketch phase -------------------------------------------------------------
# Sketches are first-class references: a profile drawn inside an open sketch
# must SURVIVE the EXTRUDE built from it (untagged profiles stay consumed —
# the 3D phase above already proves that: EXTRUDE left exactly 1 entity).
count_before_sketch="$(count)"

gexec "sketch: SKETCH NEW smoke" "SKETCH NEW smoke"
gexec "sketch: RECT inside sketch" "RECT 200,0 240,30"
sketch_rect_id="$(max_id)"
assert_eq "sketch: rect added" "$((count_before_sketch + 1))" "$(count)"
gexec "sketch: SKETCH CLOSE" "SKETCH CLOSE"

gexec "sketch: EXTRUDE tagged profile" "EXTRUDE 15 $sketch_rect_id"
assert_eq "sketch: profile survived EXTRUDE" "$((count_before_sketch + 2))" "$(count)"
assert_eq "sketch: two solids now" 2 "$(wc -w <<<"$(solid_ids)")"

# The kept profile is reusable: extrude it AGAIN (no sketch->solid dependency).
gexec "sketch: EXTRUDE same profile again" "EXTRUDE 30 $sketch_rect_id"
assert_eq "sketch: profile still there" "$((count_before_sketch + 3))" "$(count)"
assert_eq "sketch: three solids now" 3 "$(wc -w <<<"$(solid_ids)")"

gexec "sketch: UNDO extrude #2" "UNDO"
gexec "sketch: UNDO extrude #1" "UNDO"
assert_eq "sketch: undos restore counts" "$((count_before_sketch + 1))" "$(count)"

# --- sub-shape ops phase (PUSHPULL / SHELLOPEN / SPLITFACE by index) ----------
# The headless twins of the direct-modeling ops: faces addressed by the
# INSPECT indices. A fresh 40x30x10 box keeps the geometry predictable.
gexec "subshape: RECT profile" "RECT 300,0 340,30"
pp_rect_id="$(max_id)"
gexec "subshape: EXTRUDE box h=10" "EXTRUDE 10 $pp_rect_id"
pp_box_id="$(max_id)"

face_idx_at_z() { # inspect-json z -> index of the planar face at that height
    jget "$1" "([m.split()[1].rstrip(':') for m in d['result']['messages'] if m.startswith('face ') and 'plane' in m and m.endswith(',$2)')] or [''])[0]"
}
insp="$(rpc exec "INSPECT $pp_box_id Faces")"
top_idx="$(face_idx_at_z "$insp" "10.0")"
bot_idx="$(face_idx_at_z "$insp" "0.0")"
assert_ne "subshape: top face index found" "" "$top_idx"
assert_ne "subshape: bottom face index found" "" "$bot_idx"

# PUSHPULL +5 on the top face: the top plane moves from z=10 to z=15.
gexec "subshape: PUSHPULL +5 top face" "PUSHPULL 5 $top_idx $pp_box_id"
assert_ne "subshape: top face now at z=15" "" \
    "$(face_idx_at_z "$(rpc exec "INSPECT $pp_box_id Faces")" "15.0")"
gexec "subshape: UNDO pushpull" "UNDO"
assert_ne "subshape: undo puts top back to z=10" "" \
    "$(face_idx_at_z "$(rpc exec "INSPECT $pp_box_id Faces")" "10.0")"

# SHELLOPEN top+bottom: the box becomes a rectangular tube (6 -> 10 faces).
gexec "subshape: SHELLOPEN t=2 top+bottom" \
    "SHELLOPEN 2 $pp_box_id $top_idx $bot_idx"
assert_eq "subshape: tube has 10 faces" 10 "$(jget \
    "$(rpc exec "INSPECT $pp_box_id Faces")" \
    "sum(1 for m in d['result']['messages'] if m.startswith('face '))")"
gexec "subshape: UNDO shellopen" "UNDO"
assert_eq "subshape: undo restores 6 faces" 6 "$(jget \
    "$(rpc exec "INSPECT $pp_box_id Faces")" \
    "sum(1 for m in d['result']['messages'] if m.startswith('face '))")"

# SPLITFACE: a cylinder wall through the box replaces it with 2 pieces.
gexec "subshape: CIRCLE tool profile" "CIRCLE 320,15 8"
gexec "subshape: EXTRUDE tool cylinder" "EXTRUDE 30 $(max_id)"
pp_tool_id="$(max_id)"
wall_idx="$(jget "$(rpc exec "INSPECT $pp_tool_id Faces")" \
    "([m.split()[1].rstrip(':') for m in d['result']['messages'] if m.startswith('face ') and 'cylinder' in m] or [''])[0]")"
assert_ne "subshape: cylinder wall index found" "" "$wall_idx"
solids_before="$(wc -w <<<"$(solid_ids)")"
gexec "subshape: SPLITFACE cylinder wall" \
    "SPLITFACE $pp_tool_id $wall_idx $pp_box_id"
assert_eq "subshape: box replaced by 2 pieces" "$((solids_before + 1))" \
    "$(wc -w <<<"$(solid_ids)")"
gexec "subshape: UNDO splitface" "UNDO"
assert_eq "subshape: undo restores solid count" "$solids_before" \
    "$(wc -w <<<"$(solid_ids)")"

# --- edge ops / MATE / DRAFT phase (index-addressed parity commands) ----------
# Same box (pp_box_id, 40x30x10 restored by the undos) and cylinder tool.

cyl_face_count() { # solid-id -> number of cylindrical faces
    jget "$(rpc exec "INSPECT $1 Faces")" \
        "sum(1 for m in d['result']['messages'] if m.startswith('face ') and 'cylinder' in m)"
}
face_count() { # solid-id -> number of faces
    jget "$(rpc exec "INSPECT $1 Faces")" \
        "sum(1 for m in d['result']['messages'] if m.startswith('face '))"
}
face_area_at_z() { # inspect-json z -> area of the planar face at that height
    jget "$1" "([m.split('area=')[1].split()[0] for m in d['result']['messages'] if m.startswith('face ') and 'plane' in m and m.endswith(',$2)')] or [''])[0]"
}

# FILLETEDGES r=3 on edge 0: the box gains a cylindrical fillet face.
gexec "edgeops: FILLETEDGES r=3 edge 0" "FILLETEDGES 3 $pp_box_id 0"
assert_eq "edgeops: fillet adds a cylinder face" 1 "$(cyl_face_count "$pp_box_id")"
gexec "edgeops: UNDO fillet" "UNDO"
assert_eq "edgeops: undo removes the fillet" 0 "$(cyl_face_count "$pp_box_id")"

# CHAMFEREDGES d=3 on edge 0: one extra PLANAR face (6 -> 7).
gexec "edgeops: CHAMFEREDGES d=3 edge 0" "CHAMFEREDGES 3 $pp_box_id 0"
assert_eq "edgeops: chamfer adds a face" 7 "$(face_count "$pp_box_id")"
gexec "edgeops: UNDO chamfer" "UNDO"
assert_eq "edgeops: undo restores 6 faces" 6 "$(face_count "$pp_box_id")"

# DRAFT 3 deg on the 4 side faces (planar faces with centroid z=5.0); pull +Z,
# neutral plane at the solid's zMin. The top face area moves off 1200.0.
insp="$(rpc exec "INSPECT $pp_box_id Faces")"
side_idxs="$(jget "$insp" \
    "' '.join(m.split()[1].rstrip(':') for m in d['result']['messages'] if m.startswith('face ') and 'plane' in m and m.endswith(',5.0)'))")"
assert_eq "draft: found 4 side faces" 4 "$(wc -w <<<"$side_idxs")"
top_area_before="$(face_area_at_z "$insp" "10.0")"
gexec "draft: DRAFT 3deg on sides" "DRAFT 3 $pp_box_id $side_idxs"
assert_ne "draft: top face area changed" "$top_area_before" \
    "$(face_area_at_z "$(rpc exec "INSPECT $pp_box_id Faces")" "10.0")"
gexec "draft: UNDO draft" "UNDO"
assert_eq "draft: undo restores top area" "$top_area_before" \
    "$(face_area_at_z "$(rpc exec "INSPECT $pp_box_id Faces")" "10.0")"

# MATE: snap the cylinder's top face (z=30) flat onto the box top (z=10) —
# the mated face lands coincident on the box top plane.
box_top_idx="$(face_idx_at_z "$(rpc exec "INSPECT $pp_box_id Faces")" "10.0")"
cyl_top_idx="$(face_idx_at_z "$(rpc exec "INSPECT $pp_tool_id Faces")" "30.0")"
assert_ne "mate: box top index found" "" "$box_top_idx"
assert_ne "mate: cylinder top index found" "" "$cyl_top_idx"
gexec "mate: MATE cyl top onto box top" \
    "MATE $pp_tool_id $cyl_top_idx $pp_box_id $box_top_idx"
assert_ne "mate: mated face sits at z=10" "" \
    "$(face_idx_at_z "$(rpc exec "INSPECT $pp_tool_id Faces")" "10.0")"
gexec "mate: UNDO mate" "UNDO"
assert_ne "mate: undo puts the face back to z=30" "" \
    "$(face_idx_at_z "$(rpc exec "INSPECT $pp_tool_id Faces")" "30.0")"

# --- STL export --------------------------------------------------------------
out="$(rpc save "$TMP/smoke.vkd")"
assert_eq "stl: save .vkd" True "$(jget "$out" "d['result'].get('ok')")"

out="$("$CLI" export "$TMP/smoke.vkd" "$TMP/smoke.stl" 2>/dev/null)"
assert_eq "stl: export ok" True "$(jget "$out" "d.get('ok')")"

stl_check="$(python3 - "$TMP/smoke.stl" <<'PYEOF'
import os, struct, sys
path = sys.argv[1]
try:
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        head = f.read(84)
    if size == 0:
        print("empty file")
    elif head[:5] == b"solid" and b"facet" in open(path, "rb").read():
        print("ok-ascii")
    elif size >= 84:
        (ntri,) = struct.unpack("<I", head[80:84])
        if ntri > 0 and size == 84 + 50 * ntri:
            print("ok-binary %d triangles" % ntri)
        else:
            print("bad binary: %d triangles, %d bytes" % (ntri, size))
    else:
        print("too short: %d bytes" % size)
except OSError as e:
    print("unreadable: %s" % e)
PYEOF
)"
if [[ "$stl_check" == ok-* ]]; then record PASS "stl: header + size" "$stl_check"
else record FAIL "stl: header + size" "$stl_check"; fi

# --- GUI-path exports (File>Export / IPC "export" verb) -----------------------
out="$(rpc export "$TMP/smoke.step")"
assert_eq "export: STEP via GUI ok" True "$(jget "$out" "d['result'].get('ok')")"
if head -c 12 "$TMP/smoke.step" 2>/dev/null | grep -q "ISO-10303"; then
  record PASS "export: STEP header" "ISO-10303"
else
  record FAIL "export: STEP header" "missing ISO-10303"
fi
out="$(rpc export "$TMP/smoke.dxf")"
assert_eq "export: DXF via GUI ok" True "$(jget "$out" "d['result'].get('ok')")"
if grep -q "ENTITIES" "$TMP/smoke.dxf" 2>/dev/null; then
  record PASS "export: DXF has ENTITIES" "yes"
else
  record FAIL "export: DXF has ENTITIES" "no"
fi

# --- Gerber kit phase (real S5M0PCBA kit; SKIPs when pcb-ref is absent) -------
# Opens the fab-output directory headless through the IPC "open" verb (one
# layer per file, ONE transaction), checks the layer list, proves the render
# is non-empty and stable (LPC compositing is deterministic), and that a
# single UNDO restores an empty document (screenshots diff like the 3D
# phase); REDO brings everything back.
KIT_DIR=/home/lex/computer/pcb-ref/S5M0PCBA
if [[ -d "$KIT_DIR" ]]; then
    out="$(rpc open "$KIT_DIR")"
    assert_eq "kit: open S5M0PCBA (IPC)" True "$(jget "$out" "d['result'].get('ok')")"
    kit_count="$(count)"
    assert_ge "kit: entities imported" "$kit_count" 1000

    layers="$(jget "$(rpc query layers)" "','.join(l['name'] for l in d['result']['layers'])")"
    for want in Top-Copper Bottom-Copper Top-Mask Bottom-Mask Top-Silk \
                Bottom-Silk Top-Paste Bottom-Paste Drill Drill-NPTH; do
        if [[ ",$layers," == *",$want,"* ]]; then
            record PASS "kit: layer $want" "present"
        else
            record FAIL "kit: layer $want" "missing from: $layers"
        fi
    done

    shot "kit: screenshot K (board)" "$TMP/kit_a.png"
    shot "kit: screenshot K' (again)" "$TMP/kit_b.png"
    assert_le "kit: render stable (K' == K)" \
        "$(img_hash_dist "$TMP/kit_a.png" "$TMP/kit_b.png")" "$SAME_HASH_MAX"

    gexec "kit: UNDO whole kit" "UNDO"
    assert_eq "kit: ONE undo empties the document" 0 "$(count)"
    shot "kit: screenshot E (empty)" "$TMP/kit_empty.png"
    assert_ge "kit: E differs from K (board gone)" \
        "$(img_pixel_bp "$TMP/kit_a.png" "$TMP/kit_empty.png")" "$DIFF_PIXELS_MIN"

    gexec "kit: REDO whole kit" "REDO"
    assert_eq "kit: REDO restores every entity" "$kit_count" "$(count)"
    shot "kit: screenshot K'' (redo)" "$TMP/kit_c.png"
    assert_le "kit: K'' == K (render restored)" \
        "$(img_hash_dist "$TMP/kit_a.png" "$TMP/kit_c.png")" "$SAME_HASH_MAX"

    # --- G2 layer stack: BOARDVIEW presets + LAYER alpha ---------------------
    # All comparisons on CLEAN captures (geometry only). TOP dims the
    # bottom-side layers; BOTTOM dims the top side AND mirrors the view
    # left-right (solder side); ALL restores a render identical to the
    # initial one. LAYER ... ALPHA is the manual per-layer knob.
    layer_alpha() { # layer-name -> alpha from query layers
        jget "$(rpc query layers)" \
            "[l['alpha'] for l in d['result']['layers'] if l['name']=='$1'][0]"
    }
    rpc screenshot "$TMP/bv_init.png" clean >/dev/null

    gexec "stack: BOARDVIEW TOP" "BOARDVIEW TOP"
    rpc screenshot "$TMP/bv_top.png" clean >/dev/null
    assert_ge "stack: TOP dims the bottom side" \
        "$(img_pixel_bp "$TMP/bv_init.png" "$TMP/bv_top.png")" "$DIFF_PIXELS_MIN"
    assert_eq "stack: TOP -> Bottom-Copper alpha 25" 25 "$(layer_alpha Bottom-Copper)"
    assert_eq "stack: TOP -> Top-Copper opaque" 100 "$(layer_alpha Top-Copper)"
    assert_eq "stack: TOP -> Drill stays opaque" 100 "$(layer_alpha Drill)"

    gexec "stack: BOARDVIEW BOTTOM" "BOARDVIEW BOTTOM"
    rpc screenshot "$TMP/bv_bottom.png" clean >/dev/null
    assert_ge "stack: BOTTOM differs from TOP (mirror)" \
        "$(img_pixel_bp "$TMP/bv_top.png" "$TMP/bv_bottom.png")" "$DIFF_PIXELS_MIN"
    assert_eq "stack: BOTTOM -> Top-Copper alpha 25" 25 "$(layer_alpha Top-Copper)"
    assert_eq "stack: BOTTOM -> Bottom-Copper opaque" 100 "$(layer_alpha Bottom-Copper)"

    gexec "stack: BOARDVIEW ALL" "BOARDVIEW ALL"
    rpc screenshot "$TMP/bv_all.png" clean >/dev/null
    assert_le "stack: ALL == initial render" \
        "$(img_hash_dist "$TMP/bv_init.png" "$TMP/bv_all.png")" "$SAME_HASH_MAX"

    gexec "stack: LAYER Top-Copper ALPHA 30" "LAYER Top-Copper ALPHA 30"
    assert_eq "stack: LAYER alpha visible in query" 30 "$(layer_alpha Top-Copper)"
    rpc screenshot "$TMP/bv_a30.png" clean >/dev/null
    assert_ge "stack: alpha 30 changes the render" \
        "$(img_pixel_bp "$TMP/bv_all.png" "$TMP/bv_a30.png")" "$DIFF_PIXELS_MIN"
    gexec "stack: LAYER Top-Copper ALPHA 100" "LAYER Top-Copper ALPHA 100"
    rpc screenshot "$TMP/bv_a100.png" clean >/dev/null
    assert_le "stack: alpha 100 back to initial" \
        "$(img_hash_dist "$TMP/bv_init.png" "$TMP/bv_a100.png")" "$SAME_HASH_MAX"

    # --- G2 measuring: MINDIST clearance + a pad-to-pad dimension -------------
    # Ids and the by-hand expectation come from the live document itself:
    # first two drill circles (edge-to-edge = |c1-c2| - r1 - r2, computed
    # here in python), first two pad inserts > 1 mm apart for the dimension.
    ents_json="$(rpc query entities)"
    read -r DID_A DID_B DEXP P0 P1 PMID <<<"$(printf '%s' "$ents_json" | python3 -c "
import json, math, sys
d = json.load(sys.stdin)
es = d['result']['entities']
cir = sorted((e for e in es if e['type'] == 'circle'), key=lambda e: e['id'])
a, b = cir[0], cir[1]
(ax, ay), ar = a['geom']['center'], a['geom']['radius']
(bx, by), br = b['geom']['center'], b['geom']['radius']
ins = sorted((e for e in es if e['type'] == 'insert'), key=lambda e: e['id'])
p0 = ins[0]
p1 = next(p for p in ins
          if math.hypot(p['geom']['pos'][0] - p0['geom']['pos'][0],
                        p['geom']['pos'][1] - p0['geom']['pos'][1]) > 1.0)
x0, y0 = p0['geom']['pos']
x1, y1 = p1['geom']['pos']
print(a['id'], b['id'], round(math.hypot(bx - ax, by - ay) - ar - br, 6),
      f'{x0:.6f},{y0:.6f}', f'{x1:.6f},{y1:.6f}',
      f'{(x0 + x1) / 2 + 5:.6f},{(y0 + y1) / 2 + 5:.6f}')
")"
    shot "measure: canvas before MINDIST" "$TMP/mind_pre.png"
    out="$(rpc exec "MINDIST $DID_A $DID_B")"
    assert_eq "measure: MINDIST drills == by hand" "$DEXP" \
        "$(jget "$out" "round(next(json.loads(m)['mindist']['mm'] for m in d['result']['messages'] if m.startswith('{')), 6)")"
    assert_eq "measure: MINDIST method is exact" exact \
        "$(jget "$out" "next(json.loads(m)['mindist']['method'] for m in d['result']['messages'] if m.startswith('{'))")"
    shot "measure: canvas after MINDIST" "$TMP/mind_post.png"
    assert_ge "measure: witness overlay visible" \
        "$(img_px_changed "$TMP/mind_pre.png" "$TMP/mind_post.png")" 10

    kit_n="$(count)"
    gexec "measure: DIMALIGNED pad -> pad" "DIMALIGNED $P0 $P1 $PMID"
    assert_eq "measure: dimension entity added" "$((kit_n + 1))" "$(count)"
    rpc screenshot "$TMP/dim_a.png" clean >/dev/null
    rpc screenshot "$TMP/dim_b.png" clean >/dev/null
    assert_ge "measure: dimension visible (clean)" \
        "$(img_px_changed "$TMP/bv_a100.png" "$TMP/dim_a.png")" 50
    assert_eq "measure: dimensioned capture stable" \
        0 "$(img_px_changed "$TMP/dim_a.png" "$TMP/dim_b.png")"
    gexec "measure: UNDO the dimension" "UNDO"
    assert_eq "measure: entity count restored" "$kit_n" "$(count)"
    rpc screenshot "$TMP/dim_undo.png" clean >/dev/null
    assert_eq "measure: undo restores the clean render" \
        0 "$(img_px_changed "$TMP/bv_a100.png" "$TMP/dim_undo.png")"

    # A LONE fab file (not a directory) opens through the same kit path
    # thanks to the content sniff -- regression for the single-.GTL IPC open.
    out="$(rpc open "$KIT_DIR/S5M0PCBA1.GTL")"
    assert_eq "kit: open lone .GTL (IPC)" True "$(jget "$out" "d['result'].get('ok')")"
    assert_ge "kit: lone .GTL entities" "$(count)" 500
else
    record SKIP "kit: Gerber kit phase" "pcb-ref kits absent on this machine"
fi

# --- Gerber LPC paint-order phase (synthetic golden, self-contained) ----------
# lpc_redraw.gbr alternates polarity 4 times: LPD plane region -> LPC corridor
# -> LPD trace REDRAWN inside the corridor -> LPC second clear punched over
# the trace -> LPD pad flashed back. The Gerber semantics is a strict paint
# ORDER: the redrawn trace/pad must be visible INSIDE their clears, and the
# second clear must eat the first trace where they overlap. A two-pass
# composition (all darks then all clears, the classic bug) fails the
# trace-redrawn/pad-redrawn probes; ignoring later clears fails 2nd-clear.
# Probed on a CLEAN capture (screenshot ... clean = no grid/axes/crosshair),
# world->pixel mapped through the ink bbox (the 40x40 mm plane).
out="$(rpc open "$ROOT/tests/golden/gerber/lpc_redraw.gbr")"
assert_eq "lpc: open lpc_redraw.gbr" True "$(jget "$out" "d['result'].get('ok')")"
shot "lpc: clean capture" "$TMP/lpc.png"
rpc screenshot "$TMP/lpc.png" clean >/dev/null # overwrite with the clean one
lpc_probes="$(python3 - "$TMP/lpc.png" <<'PYEOF'
import sys
import warnings
warnings.simplefilter("ignore")
from collections import Counter
from PIL import Image
img = Image.open(sys.argv[1]).convert("L")
bg = Counter(img.getdata()).most_common(1)[0][0]
m = img.point(lambda v: 255 if abs(v - bg) > 10 else 0)
box = m.getbbox()
if box is None:
    print("empty-render")
    sys.exit(0)
x0, y0, x1, y1 = box
mp = m.load()
def probe(wx, wy):  # world mm (plane = 0..40 on both axes) -> ink?
    px = x0 + wx / 40.0 * (x1 - 1 - x0)
    py = y1 - 1 - wy / 40.0 * (y1 - 1 - y0)
    return mp[int(round(px)), int(round(py))] > 127
checks = [  # name, world x, world y, ink expected
    ("plane-ink", 20, 32, True),        # plane above the corridor
    ("corridor-clear", 7, 22.5, False), # corridor punched out of the plane
    ("trace-redrawn", 27, 20, True),    # LPD trace back INSIDE the corridor
    ("pad-redrawn", 20, 20, True),      # LPD pad back INSIDE the 2nd clear
    ("2nd-clear", 22.2, 19.5, False),   # 2nd LPC eats the 1st trace
]
print(" ".join(f"{n}={'ok' if probe(x, y) == want else 'BAD'}"
               for n, x, y, want in checks))
PYEOF
)"
for pr in $lpc_probes; do
    name="${pr%%=*}"
    if [[ "$pr" == *"=ok" ]]; then record PASS "lpc: $name" "pixel probe"
    else record FAIL "lpc: $name" "$pr (paint order broken?)"; fi
done

# ---- (5) OPTIONAL final stage: pixel diff vs gerbv on the real kits ---------
# gerber-ref-diff.sh renders the 32 layers of the two reference kits with
# both VikiCAD and gerbv and compares them (dhash + ink delta). ~12 s on
# this machine, so it rides along in every smoke run. It restarts the
# vikicad-gui unit for its own captures — keep it LAST, after every IPC
# check above. Silently skipped (counts as success) when gerbv or the
# kits are absent, mirroring the script's own guards.
if command -v gerbv >/dev/null 2>&1 && [[ -d /home/lex/computer/pcb-ref/S5M0PCBA ]]; then
    if "$ROOT/scripts/gerber-ref-diff.sh" >"$TMP/refdiff.log" 2>&1; then
        record PASS "refdiff: layers vs gerbv" \
            "$(grep -c '^PASS' "$TMP/refdiff.log") PASS rows"
    else
        record FAIL "refdiff: layers vs gerbv" \
            "$(grep -c '^FAIL' "$TMP/refdiff.log") FAIL rows — see $TMP/refdiff.log"
    fi
else
    record SKIP "refdiff: layers vs gerbv" "gerbv or pcb-ref kits absent"
fi

# ---- (6) report -------------------------------------------------------------

print_table
[[ "$FAILS" -eq 0 ]]
