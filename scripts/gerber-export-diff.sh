#!/usr/bin/env bash
# gerber-export-diff.sh — THE G3 fabrication truth, as a harness stage:
# for every layer of the real kits, gerbv renders the ORIGINAL file and the
# file EXPORTED by VikiCAD (import -> .vkd -> export); both PNGs must be
# visually identical. gerbv judges our WRITING — our renderer is entirely
# out of the loop, so the thresholds are TIGHT (gerbv vs gerbv):
#   dhash < 30/1024 and |ink delta| <= 1 percent point
# (observed on 2026-07-17: dhash <= 1, ink delta 0 on all 30 layers).
#
# Pairing is BY SOURCE LAYER, not by file extension: the original GKO of
# S5M0PCBB is a keepout ZONE while our exported .GKO carries the ELECTED
# Outline (GM1) — kit-mapped layers compare against the kit export, layers
# without a kit extension (Keepout, Mech-N, Top/Bottom-Pads) against a
# single-layer export (mono_<layer>.GBR). Both drill layers of a kit merge
# into ONE exported .TXT, exactly like the original Altium drill file.
# Valid-but-empty originals (header-only GKO/GM1) are not re-imported, so
# they have no exported counterpart: PASS "empty original" when gerbv sees
# no ink in them, FAIL otherwise.
#
# Exit 0 with "SKIP" when gerbv or the private kits are absent.
# Pure CLI + gerbv: no GUI, no systemd unit.

set -uo pipefail

if ! command -v gerbv >/dev/null 2>&1; then
    echo "SKIP: gerbv non installe (sudo apt install gerbv)"
    exit 0
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLI="$ROOT/build/debug/cli/vikicad-cli"
TMP="$(mktemp -d /tmp/vikicad-expdiff.XXXXXX)"
KITS=(/home/lex/computer/pcb-ref/S5M0PCBA /home/lex/computer/pcb-ref/S5M0PCBB)
DPI=400
HASH_MAX=30 # dhash bits out of 1024 — gerbv vs gerbv is near-exact
INK_MAX=1   # |ink% orig - ink% export| in percent points

FAILS=0
ROWS=()
START=$SECONDS

trap 'rm -rf "$TMP"' EXIT INT TERM

record() { # status original exported-as detail
    ROWS+=("$1|$2|$3|${4:-}")
    [[ "$1" == FAIL ]] && FAILS=$((FAILS + 1))
}

print_table() {
    echo
    printf '%-4s | %-34s | %-24s | %s\n' RES ORIGINAL 'EXPORTED AS' DETAIL
    printf -- '-----+------------------------------------+--------------------------+------------------------\n'
    local row
    for row in "${ROWS[@]}"; do
        IFS='|' read -r st name exp detail <<<"$row"
        printf '%-4s | %-34s | %-24s | %s\n' "$st" "$name" "$exp" "$detail"
    done
    echo
    local secs=$((SECONDS - START))
    if [[ "$FAILS" -eq 0 ]]; then
        echo "gerber-export-diff: ALL GREEN (${#ROWS[@]} layers, ${secs}s)"
    else
        echo "gerber-export-diff: $FAILS FAILED out of ${#ROWS[@]} layers (${secs}s)"
    fi
}

# gerbv-vs-gerbv ink comparison: binarize both renders, crop to the ink
# bbox, resize to 256x256, 32x32 dhash + ink-density delta.
# Prints "hash_dist ink_delta", "BOTHEMPTY -", "ORIGEMPTY -" or "ERR why".
cmp_pngs() { # orig.png exported.png
    python3 - "$1" "$2" <<'PYEOF'
import sys
import warnings
warnings.simplefilter("ignore")
from PIL import Image

def mask(path):
    img = Image.open(path).convert("L")
    m = img.point(lambda v: 255 if v > 48 else 0)
    box = m.getbbox()
    if box is None:
        return None, 0.0
    m = m.crop(box).resize((256, 256), Image.LANCZOS)
    px = list(m.getdata())
    return m, sum(1 for v in px if v > 127) / len(px)

def dhash(img, size=32):
    small = img.resize((size + 1, size), Image.LANCZOS)
    px = list(small.getdata())
    bits = []
    for row in range(size):
        line = px[row * (size + 1):(row + 1) * (size + 1)]
        bits.extend(1 if line[c] < line[c + 1] else 0 for c in range(size))
    return bits

a, ia = mask(sys.argv[1])
b, ib = mask(sys.argv[2])
if a is None and b is None:
    print("BOTHEMPTY -")
elif a is None or b is None:
    print("ERR", "original-empty-but-export-not" if a is None
          else "export-empty-but-original-not")
else:
    ha, hb = dhash(a), dhash(b)
    print(sum(x != y for x, y in zip(ha, hb)), round(abs(ia - ib) * 100))
PYEOF
}

render() { # gerber-file out.png -> 0/1
    gerbv --export=png --output="$2" --dpi="$DPI" --border=0 \
        --background='#000000' --foreground='#FFFFFF' "$1" >/dev/null 2>&1 \
        && [[ -s "$2" ]]
}

[[ -x "$CLI" ]] || { echo "missing $CLI (build first)"; exit 1; }

any_kit=""
for kit in "${KITS[@]}"; do [[ -d "$kit" ]] && any_kit=1; done
if [[ -z "$any_kit" ]]; then
    echo "SKIP: pcb-ref kits absent"
    exit 0
fi

for kit in "${KITS[@]}"; do
    kb="$(basename "$kit")"
    [[ -d "$kit" ]] || { record SKIP "$kb" "-" "kit absent"; continue; }
    vkd="$TMP/$kb.vkd"
    out="$TMP/out-$kb"
    mkdir -p "$out"

    imp="$("$CLI" import "$kit" --save-as "$vkd" 2>/dev/null)"
    if [[ "$(printf '%s' "$imp" | python3 -c \
        'import json,sys; print(json.load(sys.stdin).get("ok"))' 2>/dev/null)" != "True" ]]; then
        record FAIL "$kb" "-" "import failed"
        continue
    fi
    exp="$("$CLI" export "$vkd" "$out/" 2>/dev/null)"
    if [[ "$(printf '%s' "$exp" | python3 -c \
        'import json,sys; print(json.load(sys.stdin).get("ok"))' 2>/dev/null)" != "True" ]]; then
        record FAIL "$kb" "-" "kit export failed"
        continue
    fi

    # original file -> its imported layers, and layer -> exported kit file.
    # Layers the kit export skips (no kit extension) get a single-layer
    # export right here; the drill pair shares one .TXT like the original.
    pairs="$(python3 - "$imp" "$exp" <<'PYEOF'
import json, sys
imp, exp = json.loads(sys.argv[1]), json.loads(sys.argv[2])
bylayer = {}
for f in exp["result"]["files"]:
    for l in f["layers"]:
        bylayer[l] = f["path"]
srcs = {}
for fl in imp["result"]["fileLayers"]:
    srcs.setdefault(fl["file"], []).append(fl["layer"])
for src, layers in srcs.items():
    paths = {bylayer.get(l) for l in layers}
    if len(paths) == 1 and None not in paths:
        print(f"{src}\t{paths.pop()}")
    else:  # unmapped in the kit export -> one mono export per layer
        for l in layers:
            print(f"{src}\tMONO:{l}")
PYEOF
)"

    while IFS=$'\t' read -r src target; do
        [[ -n "$src" ]] || continue
        name="$kb/$src"
        if [[ "$target" == MONO:* ]]; then
            layer="${target#MONO:}"
            target="$out/mono_$layer.GBR"
            mono="$("$CLI" export "$vkd" "$target" --layer "$layer" 2>/dev/null)"
            if [[ "$(printf '%s' "$mono" | python3 -c \
                'import json,sys; print(json.load(sys.stdin).get("ok"))' 2>/dev/null)" != "True" ]]; then
                record FAIL "$name" "mono:$layer" "single-layer export failed"
                continue
            fi
        fi
        ref="$TMP/orig_${kb}_${src}.png"
        got="$TMP/exp_${kb}_$(basename "$target").png"
        if ! render "$kit/$src" "$ref"; then
            record FAIL "$name" "$(basename "$target")" "gerbv failed on ORIGINAL"
            continue
        fi
        if ! render "$target" "$got"; then
            record FAIL "$name" "$(basename "$target")" "gerbv failed on EXPORT"
            continue
        fi
        read -r hdist inkdelta <<<"$(cmp_pngs "$ref" "$got")"
        if [[ "$hdist" == "BOTHEMPTY" ]]; then
            record PASS "$name" "$(basename "$target")" "both empty"
        elif [[ "$hdist" == "ERR" ]]; then
            record FAIL "$name" "$(basename "$target")" "comparison error: $inkdelta"
        elif [[ "$hdist" -lt "$HASH_MAX" && "$inkdelta" -le "$INK_MAX" ]]; then
            record PASS "$name" "$(basename "$target")" "dhash=$hdist ink-delta=$inkdelta%"
        else
            record FAIL "$name" "$(basename "$target")" \
                "dhash=$hdist (max <$HASH_MAX) ink-delta=$inkdelta% (max $INK_MAX)"
        fi
    done <<<"$pairs"

    # Fab files of the kit that were NOT imported: valid only when EMPTY
    # (Altium ships header-only GKO/GM1 — nothing to re-export).
    for f in "$kit"/*; do
        base="$(basename "$f")"
        ext="${base##*.}"
        case "${ext^^}" in
            G*[A-Z0-9]) ;;
            TXT) head -c 4096 "$f" | grep -q '^M48' || continue ;;
            *) continue ;;
        esac
        grep -qF "$base" <<<"$pairs" && continue
        ref="$TMP/orig_only_${kb}_${base}.png"
        if render "$f" "$ref" && [[ -n "$(python3 - "$ref" <<'PYEOF'
import sys, warnings
warnings.simplefilter("ignore")
from PIL import Image
img = Image.open(sys.argv[1]).convert("L")
print("INK" if img.point(lambda v: 255 if v > 48 else 0).getbbox() else "")
PYEOF
)" ]]; then
            record FAIL "$kb/$base" "-" "original has ink but was never imported"
        else
            record PASS "$kb/$base" "-" "empty original (not re-exported, by design)"
        fi
    done
done

print_table
[[ "$FAILS" -eq 0 ]]
