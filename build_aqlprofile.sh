#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/projects/aqlprofile"
BUILD_DIR="$SCRIPT_DIR/build/aqlprofile"
INSTALL_DIR="$ROCM_PATH"

cmake -G Ninja -S "$SRC_DIR" -B "$BUILD_DIR" \
  -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DGPU_TARGETS=gfx1151 \
  -DAQLPROFILE_BUILD_TESTS=ON \
  -DAQLPROFILE_BUILD_INTEGERATION_TESTS=OFF
cmake --build "$BUILD_DIR"
cmake --install "$BUILD_DIR"

"$SCRIPT_DIR/sync_rocm_sdk_links.py" core "$INSTALL_DIR" \
  libhsa-amd-aqlprofile64.so

echo "AQLprofile build complete. Artifacts installed in $INSTALL_DIR"
echo "AQLprofile tests built:"
echo "  $BUILD_DIR/src/core/tests/pm4-factory-test"
echo "  $BUILD_DIR/src/pm4/tests/sqtt-builder-test"
