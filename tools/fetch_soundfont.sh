#!/bin/sh
# Download the General MIDI SoundFont the web / Android, Linux and macOS builds play the music with (not kept in the
# repository: 32 MB). GeneralUser GS v2.0.3 by S. Christian Collins, licence: soundfonts/LICENSE-GeneralUser-GS.txt.
#   sh tools/fetch_soundfont.sh build/soundfonts/GeneralUser-GS.sf2
set -e
OUT="$1"
URL=https://raw.githubusercontent.com/mrbumpy409/GeneralUser-GS/97049183643d5fc5a9322a69c5b09efb667c6c3a/GeneralUser-GS.sf2
SHA256=9575028c7a1f589f5770fccc8cff2734566af40cd26ed836944e9a5152688cfe
sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }   # macOS: shasum
mkdir -p "$(dirname "$OUT")"
if [ -f "$OUT" ] && [ "$(sha256 "$OUT")" = "$SHA256" ]; then exit 0; fi
curl -fsSL -o "$OUT.part" "$URL"
[ "$(sha256 "$OUT.part")" = "$SHA256" ] || { echo "SoundFont download: checksum mismatch" >&2; rm -f "$OUT.part"; exit 1; }
mv "$OUT.part" "$OUT"
