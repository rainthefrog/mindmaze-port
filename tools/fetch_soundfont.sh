#!/bin/sh
# Download the General MIDI SoundFont the web / Android build plays the music with (not kept in the repository:
# 32 MB). GeneralUser GS v2.0.3 by S. Christian Collins, licence: soundfonts/LICENSE-GeneralUser-GS.txt.
#   sh tools/fetch_soundfont.sh build/soundfonts/GeneralUser-GS.sf2
set -e
OUT="$1"
URL=https://raw.githubusercontent.com/mrbumpy409/GeneralUser-GS/97049183643d5fc5a9322a69c5b09efb667c6c3a/GeneralUser-GS.sf2
SHA256=9575028c7a1f589f5770fccc8cff2734566af40cd26ed836944e9a5152688cfe
mkdir -p "$(dirname "$OUT")"
if [ -f "$OUT" ] && echo "$SHA256  $OUT" | sha256sum -c - >/dev/null 2>&1; then exit 0; fi
curl -fsSL -o "$OUT.part" "$URL"
echo "$SHA256  $OUT.part" | sha256sum -c - >/dev/null || { echo "SoundFont download: checksum mismatch" >&2; rm -f "$OUT.part"; exit 1; }
mv "$OUT.part" "$OUT"
