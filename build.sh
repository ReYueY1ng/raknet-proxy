#!/bin/sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "[build] Configuring CMake..."
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release

echo "[build] Building..."
cmake --build build --target raknet_proxy -j$(nproc)

echo "[build] Copying binary..."
cp build/raknet_proxy .

echo "[build] Done: raknet_proxy"
