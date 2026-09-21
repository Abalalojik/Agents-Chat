# Linux build check for Agents Chat: an Ubuntu toolchain with the libraries the Linux port
# will use (libcurl, libsecret, SDL2 + OpenGL). The sources are mounted read-only at run time.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential g++ cmake ninja-build git pkg-config ca-certificates curl xz-utils \
        libcurl4-openssl-dev libssl-dev libsecret-1-dev libsdl2-dev libgl-dev \
    && rm -rf /var/lib/apt/lists/*

# Same pinned third-party versions as CMakeLists.txt (FetchContent).
RUN mkdir -p /deps \
    && curl -fsSL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz | tar -xz -C /deps \
    && mv /deps/imgui-1.92.9b /deps/imgui \
    && curl -fsSL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz | tar -xJ -C /deps \
    && mv /deps/json /deps/nlohmann_json

WORKDIR /work
