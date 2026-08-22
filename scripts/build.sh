#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT/build"

echo "[build] configuring C++ targets..."
cmake -S "$ROOT/backend" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release

echo "[build] building C++ targets..."
cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "[build] installing backend node dependencies..."
(cd "$ROOT/backend/api-server" && npm install)
(cd "$ROOT/backend/shell-app" && npm install)

echo "[build] complete."
