#!/usr/bin/env bash
# Regenerate the README banner from the screenshots in docs/screenshots/:
# the STX menu bar app, and the card's inputs and outputs in Sound preferences.
#
#   ./scripts/make-banner.sh [output.png]     # default: docs/banner.png
#
# Needs ImageMagick (`brew install imagemagick`). The version in the header
# comes from VERSION in the Makefile. Same layout idea as the ezgb banner:
# black background, Menlo labels in one accent colour, black gutters.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/docs/banner.png}"
SHOTS="$ROOT/docs/screenshots"
FONT="${BANNER_FONT:-/System/Library/Fonts/Menlo.ttc}"
GOLD='#e8b030'                  # the STX's gold RCA jacks
PANEL_H=360                     # every panel is scaled to this height
GUTTER=16
VERSION="$(awk -F':= *' '/^VERSION/{print $2}' "$ROOT/Makefile" | tr -d '[:space:]')"

command -v magick >/dev/null || { echo "error: ImageMagick not found (brew install imagemagick)"; exit 1; }
for f in stx-menu sound-input sound-output; do
  [ -f "$SHOTS/$f.png" ] || { echo "error: missing $SHOTS/$f.png"; exit 1; }
done

TMP="$(mktemp -d /tmp/stx-banner.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

# --- panels: crop, then scale to a common height --------------------------
# Sound preferences shots: drop the transparent shadow (trim to the opaque
# window), then keep the window down to the "Settings for the selected device"
# box; the system output-volume row below it is the same in both.
sound_panel() {
  local in="$1" out="$2" bbox w
  bbox="$(magick "$in" -alpha extract -threshold 99% -format '%@' info:)"
  magick "$in" -crop "$bbox" +repage "$TMP/win.png"
  w="$(magick identify -format '%w' "$TMP/win.png")"
  magick "$TMP/win.png" -crop "${w}x412+0+0" +repage -background black -flatten \
    -filter Lanczos -resize "x${PANEL_H}" "$out"
}
sound_panel "$SHOTS/sound-input.png"  "$TMP/p-input.png"
sound_panel "$SHOTS/sound-output.png" "$TMP/p-output.png"
# The menu shot is already opaque and tightly framed.
magick "$SHOTS/stx-menu.png" -background black -flatten -filter Lanczos -resize "x${PANEL_H}" \
  "$TMP/p-menu.png"

# --- a caption over each panel, then the row ------------------------------
caption() {   # caption <panel> <text> <out>
  local w ps
  w="$(magick identify -format '%w' "$1")"
  ps=22
  magick -size "${w}x44" xc:black -font "$FONT" -pointsize "$ps" -fill "$GOLD" \
    -gravity West -annotate +4+0 "$2" "$TMP/cap.png"
  magick "$TMP/cap.png" "$1" -append "$3"
}
caption "$TMP/p-menu.png"   "STX menu bar app"    "$TMP/c-menu.png"
caption "$TMP/p-input.png"  "Sound › Input"      "$TMP/c-input.png"
caption "$TMP/p-output.png" "Sound › Output"     "$TMP/c-output.png"
CH="$(magick identify -format '%h' "$TMP/c-menu.png")"
magick -size "${GUTTER}x${CH}" xc:black "$TMP/gut.png"
magick "$TMP/c-menu.png" "$TMP/gut.png" "$TMP/c-input.png" "$TMP/gut.png" "$TMP/c-output.png" \
  +append "$TMP/row.png"
RW="$(magick identify -format '%w' "$TMP/row.png")"

# --- header: title on the left, version on the right ----------------------
magick -size "${RW}x64" xc:black -font "$FONT" -pointsize 30 -fill "$GOLD" \
  -gravity West -annotate +0+0 "XONAR ESSENCE STX · macOS" \
  -gravity East -annotate +0+0 "CMI8788Driver $VERSION" "$TMP/header.png"

mkdir -p "$(dirname "$OUT")"
magick "$TMP/header.png" "$TMP/row.png" -append -bordercolor black -border 16x16 "$OUT"
echo "wrote $OUT ($(magick identify -format '%wx%h' "$OUT")px)"
