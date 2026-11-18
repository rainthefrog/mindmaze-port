# Build and test environment for mindmaze-port (nothing needs to be installed on the host).
#   docker build -t mindmaze-port-dev docker/
#   docker run --rm --memory=2g -v "$PWD:/w" -w /w mindmaze-port-dev make test
FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
      gcc libc6-dev make python3 7zip git \
      gcc-mingw-w64-i686 binutils-mingw-w64-i686 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /w
