#!/bin/sh
# Build SDL3 as a static library from its release source (checksum verified) and install it into a prefix, for the
# Linux and macOS builds (make linux / make macos link it into the program, so players install nothing).
# Used by docker/linux.Dockerfile and by the macOS job of .github/workflows/release.yml.
#   sh tools/build_sdl3.sh <prefix> [extra cmake options]
#   macOS universal: sh tools/build_sdl3.sh build/sdl3-macos "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
# On Linux, SDL loads X11 / Wayland / ALSA / PulseAudio / PipeWire / D-Bus at run time (dlopen), so the headers of
# those must be present when building (see docker/linux.Dockerfile) but the program does not depend on them.
set -e
VER=3.2.30
SHA256=4c3b09330d866dc52eb65b66259a6684ad387252ca8c57901b3a2b534eb42e3d
PREFIX="$1"
shift
[ -n "$PREFIX" ] || { echo "usage: build_sdl3.sh <prefix> [cmake options]" >&2; exit 1; }
case "$PREFIX" in /*) ;; *) PREFIX="$(pwd)/$PREFIX" ;; esac
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
curl -fsSL -o "$WORK/sdl.tar.gz" "https://github.com/libsdl-org/SDL/releases/download/release-$VER/SDL3-$VER.tar.gz"
if command -v sha256sum >/dev/null; then SUM="$(sha256sum "$WORK/sdl.tar.gz")"; else SUM="$(shasum -a 256 "$WORK/sdl.tar.gz")"; fi
[ "${SUM%% *}" = "$SHA256" ] || { echo "SDL3 download: checksum mismatch" >&2; exit 1; }
tar -xzf "$WORK/sdl.tar.gz" -C "$WORK"
cmake -S "$WORK/SDL3-$VER" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF "$@"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
cmake --build "$WORK/build" --parallel "$JOBS"
cmake --install "$WORK/build"
