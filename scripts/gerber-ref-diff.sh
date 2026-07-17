#!/usr/bin/env bash
# gerber-ref-diff.sh — visual diff of VikiCAD's Gerber rendering against
# gerbv (the reference renderer), layer by layer, on the real Altium kits.
#
# For every Gerber/Excellon layer of the reference kits:
#   1. gerbv exports the layer to PNG (CLI, white on black, fixed DPI);
#   2. VikiCAD opens the SAME single file headless (IPC "open" routes a lone
#      fab file through the kit importer) and takes a CLEAN screenshot
#      (`screenshot PATH clean` = no grid/UCS icon/crosshair overlays — they
#      used to pollute the ink bounding box and skew every comparison);
#   3. both images are normalized (binarized ink mask -> crop to the ink
#      bounding box -> common size) and compared with the same 32x32 dhash
#      used by gui-smoke.sh, plus an ink-density delta. Ink on the gerbv
#      side is "brighter than the black background"; on the VikiCAD side it
#      is "different from the canvas background color" (sampled at a corner)
#      so BLACK layers on the dark canvas (drills) count as ink too.
#
# Valid-but-EMPTY layers (Altium ships header-only GKO/GM1): both renderers
# produce an empty image -> PASS "both empty". Empty on one side only is a
# real geometry FAIL.
#
# Exit 0 with "SKIP" when gerbv is not installed (sudo needed -> Lex):
#   sudo apt install gerbv
# NOT part of gui-smoke.sh (needs gerbv + the private kits).
#
# Thresholds: calibrated on the real 2026-07-17 run over the 32 layers of
# S5M0PCBA + S5M0PCBB (values below each = observed maximum + ~30% margin).

set -uo pipefail

if ! command -v gerbv >/dev/null 2>&1; then
    echo "SKIP: gerbv non installe (sudo apt install gerbv)"
    exit 0
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLI="$ROOT/build/debug/cli/vikicad-cli"
GUI="$ROOT/build/debug/gui/vikicad"
UNIT=vikicad-gui
TMP="$(mktemp -d /tmp/vikicad-refdiff.XXXXXX)"
KITS=(/home/lex/computer/pcb-ref/S5M0PCBA /home/lex/computer/pcb-ref/S5M0PCBB)
DPI=400

# Calibrated 2026-07-17 (32 layers, all PASS). Observed dhash: median ~25,
# worst 132 (PCBB.GBL) — copper layers run high on pure edge-halo noise
# (1-2 px AA/stroke rounding around a LOT of edge length); all other layers
# are <= 40. .TXT drill layers sit at 11/40 since drills render as FILLED
# disks like gerbv (they used to be outlines: dhash 58/104 before the fix).
# Observed ink-delta: 0-2 points. Thresholds = observed max + ~30%.
REF_HASH_MAX=170 # dhash bits out of 1024 (observed max 132, PCBB.GBL copper)
INK_DELTA_MAX=3  # |ink% A - ink% B| in percent points (observed max 2)

FAILS=0
ROWS=()

cleanup() {
    systemctl --user stop "$UNIT" 2>/dev/null
    systemctl --user reset-failed "$UNIT" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

record() { # status name detail
    ROWS+=("$1|$2|$3")
    [[ "$1" == FAIL ]] && FAILS=$((FAILS + 1))
}

print_table() {
    echo
    printf '%-4s | %-42s | %s\n' RES LAYER DETAIL
    printf -- '-----+--------------------------------------------+---------------------------\n'
    local row
    for row in "${ROWS[@]}"; do
        IFS='|' read -r st name detail <<<"$row"
        printf '%-4s | %-42s | %s\n' "$st" "$name" "$detail"
    done
    echo
    if [[ "$FAILS" -eq 0 ]]; then echo "gerber-ref-diff: ALL GREEN (${#ROWS[@]} layers)"
    else echo "gerber-ref-diff: $FAILS FAILED out of ${#ROWS[@]} layers"; fi
}

jget() {
    printf '%s' "$1" | python3 -c \
        "import json,sys; d=json.load(sys.stdin); print($2)" 2>/dev/null
}

rpc() { "$CLI" connect "$@" 2>/dev/null; }

# Normalized geometry comparison (same dhash logic as gui-smoke.sh, applied
# to binarized ink masks cropped to their ink bounding boxes).
# Prints: "hash_dist ink_delta", "BOTHEMPTY -", or "ERR <reason>".
ink_cmp() { # gerbv-image vikicad-image
    python3 - "$1" "$2" <<'PYEOF'
import sys
import warnings
warnings.simplefilter("ignore")
from PIL import Image

def normalize(m):
    box = m.getbbox()
    if box is None:
        return None, 0.0
    m = m.crop(box).resize((256, 256), Image.LANCZOS)
    px = list(m.getdata())
    ink = sum(1 for v in px if v > 127) / len(px)
    return m, ink

def refmask(path):
    # gerbv export: black background, white ink.
    img = Image.open(path).convert("L")
    return normalize(img.point(lambda v: 255 if v > 48 else 0))

def vikimask(path):
    # Clean canvas capture: ink = any color that differs from the canvas
    # background (corner pixel — zoom-extents always leaves a margin). A
    # plain luminance threshold would DROP black layers (drills) drawn on
    # the dark canvas.
    img = Image.open(path).convert("RGB")
    w, h = img.size
    p = img.load()
    br, bg_, bb = p[0, 0]
    m = Image.new("L", (w, h), 0)
    mp = m.load()
    for y in range(h):
        for x in range(w):
            r, g, b = p[x, y]
            if abs(r - br) + abs(g - bg_) + abs(b - bb) > 30:
                mp[x, y] = 255
    return normalize(m)

def dhash(img, size=32):
    small = img.resize((size + 1, size), Image.LANCZOS)
    px = list(small.getdata())
    bits = []
    for row in range(size):
        line = px[row * (size + 1):(row + 1) * (size + 1)]
        bits.extend(1 if line[c] < line[c + 1] else 0 for c in range(size))
    return bits

a, ia = refmask(sys.argv[1])
b, ib = vikimask(sys.argv[2])
if a is None and b is None:
    print("BOTHEMPTY -")  # valid empty layer on both sides
elif a is None or b is None:
    print("ERR", "gerbv-empty-but-vikicad-not" if a is None
          else "vikicad-empty-but-gerbv-not")
else:
    ha, hb = dhash(a), dhash(b)
    print(sum(x != y for x, y in zip(ha, hb)), round(abs(ia - ib) * 100))
PYEOF
}

# ---- build + start the GUI (same pattern as gui-smoke.sh) -------------------
echo "gerber-ref-diff: building (no-op when up to date)..."
cmake --build "$ROOT/build/debug" --parallel "$(nproc)" >"$TMP/build.log" 2>&1 || {
    tail -20 "$TMP/build.log"; echo "build failed"; exit 1; }
[[ -x "$CLI" && -x "$GUI" ]] || { echo "missing $CLI or $GUI"; exit 1; }

systemctl --user stop "$UNIT" 2>/dev/null
systemctl --user reset-failed "$UNIT" 2>/dev/null
systemd-run --user --unit="$UNIT" --collect \
    --setenv=QT_QPA_PLATFORM=xcb \
    ${DISPLAY:+--setenv=DISPLAY="$DISPLAY"} \
    "$GUI" >/dev/null 2>&1 || { echo "systemd-run failed"; exit 1; }
up=""
for _ in $(seq 1 50); do
    [[ "$(jget "$(rpc ping)" "d['result'].get('pong')")" == "True" ]] && { up=1; break; }
    sleep 0.2
done
[[ -n "$up" ]] || { echo "GUI did not answer ping within 10 s"; exit 1; }

# ---- per-layer diff ---------------------------------------------------------
for kit in "${KITS[@]}"; do
    [[ -d "$kit" ]] || { record SKIP "$(basename "$kit")" "kit absent"; continue; }
    for f in "$kit"/*; do
        base="$(basename "$f")"
        ext="${base##*.}"
        # Fab layers only: Gerber (G**) + Excellon (.TXT that IS a drill file).
        case "${ext^^}" in
            G*[A-Z0-9]) ;;
            TXT) head -c 4096 "$f" | grep -q '^M48' || continue ;;
            *) continue ;;
        esac
        name="$(basename "$kit")/$base"

        ref="$TMP/ref_$base.png"
        got="$TMP/viki_$base.png"
        if ! gerbv --export=png --output="$ref" --dpi="$DPI" --border=0 \
                --background='#000000' --foreground='#FFFFFF' "$f" \
                >/dev/null 2>&1 || [[ ! -s "$ref" ]]; then
            record FAIL "$name" "gerbv export failed"
            continue
        fi
        out="$(rpc open "$f")"
        if [[ "$(jget "$out" "d['result'].get('ok')")" != "True" ]]; then
            record FAIL "$name" "vikicad open failed: $out"
            continue
        fi
        out="$(rpc screenshot "$got" clean)"
        if [[ "$(jget "$out" "d.get('ok')")" != "True" || ! -s "$got" ]]; then
            record FAIL "$name" "vikicad screenshot failed"
            continue
        fi

        read -r hdist inkdelta <<<"$(ink_cmp "$ref" "$got")"
        if [[ "$hdist" == "BOTHEMPTY" ]]; then
            record PASS "$name" "both empty (valid empty layer)"
        elif [[ "$hdist" == "ERR" ]]; then
            record FAIL "$name" "comparison error: $inkdelta"
        elif [[ "$hdist" -le "$REF_HASH_MAX" && "$inkdelta" -le "$INK_DELTA_MAX" ]]; then
            record PASS "$name" "dhash=$hdist ink-delta=$inkdelta%"
        else
            record FAIL "$name" "dhash=$hdist (max $REF_HASH_MAX) ink-delta=$inkdelta% (max $INK_DELTA_MAX)"
        fi
    done
done

print_table
[[ "$FAILS" -eq 0 ]]
