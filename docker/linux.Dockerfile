# Linux build environment for mindmaze-port: Ubuntu 22.04 (glibc 2.35, so the program also runs on 22.04 and later,
# SteamOS, Debian 12, Fedora ...) with SDL3 built as a static library into /opt/sdl3 (tools/build_sdl3.sh).
#   docker build -t mindmaze-port-linux -f docker/linux.Dockerfile tools/     (the context is tools/: build_sdl3.sh)
#   docker run --rm --memory=2g -v "$PWD:/w" -w /w mindmaze-port-linux make linux
# The X11 / Wayland / audio / D-Bus development packages are only needed while SDL3 is built: SDL loads those
# libraries at run time when they are present (a desktop has them).
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      gcc libc6-dev make cmake pkg-config python3 curl ca-certificates file \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev \
      libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl1-mesa-dev libgl1-mesa-dev libgles2-mesa-dev \
      libdrm-dev libgbm-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libdbus-1-dev libudev-dev libibus-1.0-dev \
    && rm -rf /var/lib/apt/lists/*
COPY build_sdl3.sh /tmp/build_sdl3.sh
RUN sh /tmp/build_sdl3.sh /opt/sdl3 && rm /tmp/build_sdl3.sh
ENV PKG_CONFIG_PATH=/opt/sdl3/lib/pkgconfig
WORKDIR /w
