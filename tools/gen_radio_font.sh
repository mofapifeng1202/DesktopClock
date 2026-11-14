#!/bin/sh
# Regenerate the project's Chinese font subsets.
#
# The generated files main/font_radio_14.c, main/font_radio_20.c and
# main/font_zh_14.c are checked in, so a normal build does NOT need Node/npm.
# Run this script after adding/changing Chinese text in:
#     main/radio.c  main/radio_ui.c  main/radio_stations.h
#     main/tv_ui.c  main/media_list.c  main/settings_ui.c  main/web_config.c
#
# Generated fonts:
#   font_radio_14/20  small subsets (source text + IPTV channel names), used by
#                     the radio / TV / settings pages
#   font_zh_14        GB2312 level-1 (3755 common hanzi) fallback, chained via
#                     .fallback from font_radio_14/20 so a channel name or a
#                     label with an unknown character never renders blank
#
# IPTV channel names are fetched from the playlist URL in main/tv_config.h
# (override with TV_URL=...) and cached in tools/tv_chars.txt so the script
# works offline afterwards.
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
CACHE="$DIR/tv_chars.txt"
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
    sub = f["name"].getDebugName(4) or ""
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

# ---- IPTV channel names (the playlist is fetched at build-host time) --------
URL="${TV_URL:-}"
if [ -z "$URL" ] && [ -f "$MAIN/tv_config.h" ]; then
    URL=$(sed -n 's/.*define[[:space:]]*TV_LIST_URL[[:space:]]*"\([^"]*\)".*/\1/p' "$MAIN/tv_config.h")
fi

PLAYLIST_CHARS=""
if [ -n "$URL" ] && curl -sS -m 25 "$URL" -o "$TMP/list.m3u" 2>/dev/null && [ -s "$TMP/list.m3u" ]; then
    PLAYLIST_CHARS=$(python3 - "$TMP/list.m3u" <<'PY'
import re, sys
txt = open(sys.argv[1], encoding="utf-8", errors="replace").read()
names = re.findall(r'tvg-name="([^"]*)"', txt) + re.findall(r'^#EXTINF[^,]*,(.*)$', txt, re.M)
chars = sorted({c for n in names for c in n if 0x4E00 <= ord(c) <= 0x9FFF})
sys.stdout.write("".join(chars))
PY
)
    if [ -n "$PLAYLIST_CHARS" ]; then
        printf '%s' "$PLAYLIST_CHARS" > "$CACHE"
        echo "playlist: fetched $(printf '%s' "$PLAYLIST_CHARS" | wc -m | tr -d ' ') CJK chars -> $(basename "$CACHE")"
    fi
fi
if [ -z "$PLAYLIST_CHARS" ] && [ -f "$CACHE" ]; then
    PLAYLIST_CHARS=$(cat "$CACHE")
    echo "playlist: using cached $(basename "$CACHE")"
fi
if [ -z "$PLAYLIST_CHARS" ]; then
    echo "warning: no playlist channel names, the TV list may show blanks" >&2
fi

# ---- charset of the small UI subsets ---------------------------------------
SUBSET_SYMS=$(python3 - "$MAIN" "$PLAYLIST_CHARS" <<'PY'
import os, sys
main, playlist = sys.argv[1], sys.argv[2]
text = playlist
for name in ("radio.c", "radio_ui.c", "radio_stations.h",
             "tv_ui.c", "media_list.c", "settings_ui.c", "web_config.c"):
    with open(os.path.join(main, name), encoding="utf-8") as f:
        text += f.read()
keep_extra = "·，。！？：；（）“”‘’—…"
chars = sorted({c for c in text
                if 0x4E00 <= ord(c) <= 0x9FFF or c in keep_extra})
sys.stdout.write("".join(chars))
PY
)

# ---- charset of the general fallback font ----------------------------------
# GB2312 level 1 (3755 common hanzi) + the playlist + the UI punctuation set,
# so any name typed into the web page still finds a glyph.
ZH_SYMS=$(python3 - "$PLAYLIST_CHARS" <<'PY'
import sys
playlist = sys.argv[1]
chars = set()
for b in range(0xB0, 0xD8):
    for t in range(0xA1, 0xFF):
        try:
            chars.add(bytes([b, t]).decode("gb2312"))
        except Exception:
            pass
chars |= set("·，。！？：；（）“”‘’—…　、")
chars |= {c for c in playlist if 0x4E00 <= ord(c) <= 0x9FFF}
sys.stdout.write("".join(sorted(chars)))
PY
)

# GB2312 level 2 (the rarer half: 霾, 郸, ...) as a second fallback layer, so
# text coming from an API (weather condition, an unusual city name) never
# renders as LVGL's "missing glyph" box.
ZH2_SYMS=$(python3 - <<'PY'
import sys
chars = []
for b in range(0xD8, 0xF8):
    for t in range(0xA1, 0xFF):
        try:
            chars.append(bytes([b, t]).decode("gb2312"))
        except Exception:
            pass
sys.stdout.write("".join(chars))
PY
)

echo "subset symbols: $(printf '%s' "$SUBSET_SYMS" | wc -m | tr -d ' ') chars"
echo "fallback symbols: $(printf '%s' "$ZH_SYMS" | wc -m | tr -d ' ') chars"

# ---- generate --------------------------------------------------------------
for SZ in 14 20; do
    lv_font_conv \
        --font "$FONT_TTF" \
        --range 0x20-0x7e \
        --symbols "$SUBSET_SYMS" \
        --size "$SZ" \
        --bpp 4 \
        --format lvgl \
        --lv-include lvgl.h \
        --no-compress \
        --force-fast-kern-format \
        -o "$MAIN/font_radio_${SZ}.c"
done

lv_font_conv \
    --font "$FONT_TTF" \
    --range 0x20-0x7e \
    --symbols "$ZH_SYMS" \
    --size 14 \
    --bpp 4 \
    --format lvgl \
    --lv-include lvgl.h \
    --no-compress \
    --force-fast-kern-format \
    -o "$MAIN/font_zh_14.c"

lv_font_conv \
    --font "$FONT_TTF" \
    --symbols "$ZH2_SYMS" \
    --size 14 \
    --bpp 4 \
    --format lvgl \
    --lv-include lvgl.h \
    --no-compress \
    --force-fast-kern-format \
    --lv-font-name font_zh2_14 \
    -o "$MAIN/font_zh2_14.c"

# Chain font_zh_14 as the fallback of the two UI fonts (lv_font_conv has no
# option for it, and the generated struct is const, so patch the C file).
python3 - "$MAIN" <<'PY'
import os, sys
main = sys.argv[1]
for name in ("font_radio_14.c", "font_radio_20.c"):
    path = os.path.join(main, name)
    src = open(path, encoding="utf-8").read()
    if "LV_FONT_DECLARE(font_zh_14)" not in src:
        src = src.replace("#ifndef FONT_RADIO_",
                          "LV_FONT_DECLARE(font_zh_14);\n\n#ifndef FONT_RADIO_", 1)
    src = src.replace(".fallback = NULL,", ".fallback = &font_zh_14,", 1)
    open(path, "w", encoding="utf-8").write(src)

# font_zh_14 itself falls back to the level-2 font for rarer characters
path = os.path.join(main, "font_zh_14.c")
src = open(path, encoding="utf-8").read()
if "LV_FONT_DECLARE(font_zh2_14)" not in src:
    src = src.replace("#ifndef FONT_ZH_14",
                      "LV_FONT_DECLARE(font_zh2_14);\n\n#ifndef FONT_ZH_14", 1)
src = src.replace(".fallback = NULL,", ".fallback = &font_zh2_14,", 1)
open(path, "w", encoding="utf-8").write(src)
print("chained font_zh_14 after the radio fonts, font_zh2_14 after it")
PY

echo "updated $MAIN/font_radio_14.c, font_radio_20.c, font_zh_14.c and font_zh2_14.c"
