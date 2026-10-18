#!/usr/bin/env bash
# build deps for debian / ubuntu / mint
set -euo pipefail

sudo apt update
sudo apt install -y \
    build-essential cmake ninja-build git pkg-config \
    libgl-dev libx11-dev libxfixes-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libsystemd-dev \
    libxtst6
