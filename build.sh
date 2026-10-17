#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/Build"
BUILD_TYPE="${BUILD_TYPE:-Release}"

cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" "$@"
cmake --build "$BUILD_DIR" -j"$(nproc)"

printf '\n==> built (type=%s): %s/relapsegtk\n' "$BUILD_TYPE" "$BUILD_DIR"
printf '==> run with %s/run.sh\n' "$SCRIPT_DIR"
