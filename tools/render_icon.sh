#!/usr/bin/env bash
# Rasterise the app icon. Usage: render_icon.sh <in.svg> <out.png> <size>
# Tries the renderers in order of how well they handle SVG; the PNGs are checked
# in, so this only has to run when the icon itself changes.
set -euo pipefail

src="$1"
dst="$2"
size="$3"

if command -v rsvg-convert >/dev/null 2>&1; then
	rsvg-convert -w "$size" -h "$size" -o "$dst" "$src"
elif command -v inkscape >/dev/null 2>&1; then
	inkscape "$src" --export-type=png --export-filename="$dst" \
		--export-width="$size" --export-height="$size" >/dev/null 2>&1
elif command -v magick >/dev/null 2>&1; then
	magick -background none -density 384 "$src" -resize "${size}x${size}" "$dst"
elif command -v convert >/dev/null 2>&1; then
	convert -background none -density 384 "$src" -resize "${size}x${size}" "$dst"
else
	echo "No SVG renderer found (rsvg-convert, inkscape or ImageMagick)." >&2
	exit 1
fi
