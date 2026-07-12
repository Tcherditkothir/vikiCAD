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
#        UNDO -> == A ; REDO -> == B ; SPLIT XY -> 2 solids ; COMBINE -> 1 ;
#        export STL -> non-empty + header/size check.
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
print(hash_dist, changed * 10000 // max(total, 1))
PYEOF
}

img_hash_dist() { img_cmp "$1" "$2" | cut -d' ' -f1; }
img_pixel_bp() { img_cmp "$1" "$2" | cut -d' ' -f2; }

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

# ---- (5) report -------------------------------------------------------------

print_table
[[ "$FAILS" -eq 0 ]]
