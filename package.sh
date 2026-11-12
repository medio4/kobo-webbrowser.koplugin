#!/bin/sh
# Assemble the KOReader plugin folder and the release zip from a finished build.
#   ./package.sh            -> dist/kobo-webbrowser.koplugin/ and dist/kobo-webbrowser.koplugin-<version>.zip
# Run ./build.sh first (it produces build/kobo-browser and build/kb-escape).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
NAME=kobo-webbrowser.koplugin
SRC="$HERE/$NAME"
DIST="$HERE/dist"
OUT="$DIST/$NAME"

for f in build/kobo-browser build/kb-escape; do
    [ -f "$HERE/$f" ] || { echo "missing $f - run ./build.sh first" >&2; exit 1; }
done
VERSION="$(sed -n 's/^ *version = "\([^"]*\)".*/\1/p' "$SRC/_meta.lua")"
[ -n "$VERSION" ] || { echo "no version in $SRC/_meta.lua" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT/bin"
cp "$SRC/_meta.lua" "$SRC/main.lua" "$OUT/"
cp "$HERE/LICENSE" "$HERE/THIRD_PARTY.md" "$OUT/"
cp "$HERE/build/kobo-browser" "$HERE/build/kb-escape" "$OUT/bin/"
cp "$HERE/device/run.sh" "$HERE/device/wifi-sleep.sh" "$HERE/device/ca-bundle.crt" "$HERE/device/adblock-hosts.txt" "$OUT/bin/"
chmod 755 "$OUT/bin/kobo-browser" "$OUT/bin/kb-escape" "$OUT/bin/run.sh" "$OUT/bin/wifi-sleep.sh"

ZIP="$DIST/$NAME-$VERSION.zip"
rm -f "$ZIP"
(cd "$DIST" && zip -q -r -X "$(basename "$ZIP")" "$NAME")
echo "$ZIP"
(cd "$DIST" && unzip -l "$(basename "$ZIP")")
