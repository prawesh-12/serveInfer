#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT/build"

echo "[build] configuring C++ targets..."
cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release

echo "[build] building C++ targets..."
cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "[build] installing api-server dependencies..."
cd "$ROOT/api-server"
npm install

echo "[build] installing shell-app dependencies..."
cd "$ROOT/shell-app"
npm install

echo "[build] complete."
