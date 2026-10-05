#!/bin/sh
# Runs only inside the private Ubuntu rootfs, never on the developer's OS.
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get -o APT::Sandbox::User=root update
apt-get -o APT::Sandbox::User=root install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config python3 curl ca-certificates \
    xz-utils bzip2 patchelf file binutils desktop-file-utils \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
    libxfixes-dev libxss-dev libxinerama-dev libxxf86vm-dev \
    libwayland-dev libxkbcommon-dev wayland-protocols libegl1-mesa-dev \
    libgl1-mesa-dev libasound2-dev libpulse-dev libudev-dev libdbus-1-dev \
    xvfb xauth
dpkg-query -W > /build-packages.txt
