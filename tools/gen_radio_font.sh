#!/bin/sh
# Regenerate the radio page's Chinese font subsets.
#
# The generated files main/font_radio_14.c and main/font_radio_20.c are checked
# in, so a normal build does NOT need Node/npm. Run this script only after
# adding/changing Chinese text in:
#     main/radio.c  main/radio_ui.c  main/radio_stations.h
#
# Requirements:
#   - lv_font_conv in PATH:  npm install -g lv_font_conv
#   - a CJK TTF/OTF/TTC:     FONT=/path/to/font.ttf ./tools/gen_radio_font.sh
#     (default: the OFL Noto Sans SC TTF bundled with LVGL's test files)
#   - fonttools:             python3 -m pip install --user fonttools (only for .ttc)
set -e

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJ="$DIR/.."
MAIN="$PROJ/main"
FONT="${FONT:-$PROJ/managed_components/lvgl__lvgl/tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf}"

if ! command -v lv_font_conv >/dev/null 2>&1; then
    echo "lv_font_conv not found. Install it with: npm install -g lv_font_conv" >&2
    exit 1
fi
if [ ! -f "$FONT" ]; then
    echo "font not found: $FONT" >&2
    echo "Set FONT=/path/to/a/CJK/font.ttf and retry." >&2
    exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# lv_font_conv wants a single font. Extract a usable face from a .ttc.
case "$FONT" in
*.ttc|*.TTC)
    python3 - "$FONT" "$TMP/font.ttf" <<'PY'
import sys
try:
    from fontTools.ttLib import TTCollection
except ImportError:
    sys.exit("fonttools missing: python3 -m pip install --user fonttools")
src, dst = sys.argv[1], sys.argv[2]
fonts = TTCollection(src).fonts
pick = None
for f in fonts:
    family = (f["name"].getDebugName(1) or "")
    sub = (f["name"].getDebugName(4) or "")
    if "SC" in family and ("Medium" in sub or "Regular" in sub):
        pick = f
        break
if pick is None:
    pick = fonts[0]
print("using face:", pick["name"].getDebugName(4))
pick.save(dst)
PY
    FONT_TTF="$TMP/font.ttf"
    ;;
*)
    FONT_TTF="$FONT"
    ;;
esac

# Every CJK glyph (plus a few punctuation marks) that appears in the radio
# sources. Comments are included on purpose: harmless and avoids surprises.
SYMS=$(python3 - "$MAIN" <<'PY'
import os, sys
main = sys.argv[1]
text = ""
for name in ("radio.c", "radio_ui.c", "radio_stations.h"):
    with open(os.path.join(main, name), encoding="utf-8") as f:
        text += f.read()
keep_extra = "·，。！？：；（）“”‘’—…"
chars = sorted({c for c in text
                if 0x4E00 <= ord(c) <= 0x9FFF or c in keep_extra})
sys.stdout.write("".join(chars))
PY
)

echo "symbols: $SYMS"
for SZ in 14 20; do
    lv_font_conv \
        --font "$FONT_TTF" \
        --range 0x20-0x7e \
        --symbols "$SYMS" \
        --size "$SZ" \
        --bpp 4 \
        --format lvgl \
        --lv-include lvgl.h \
        --no-compress \
        --force-fast-kern-format \
        -o "$MAIN/font_radio_${SZ}.c"
done
echo "updated $MAIN/font_radio_14.c and font_radio_20.c"
