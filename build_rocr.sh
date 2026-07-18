#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/projects/rocr-runtime"
BUILD_DIR="$SCRIPT_DIR/build/rocr-runtime"
ROCRTST_BUILD_DIR="$SCRIPT_DIR/build/rocr-runtime-rocrtst"
KFDTEST_BUILD_DIR="$SCRIPT_DIR/build/rocr-runtime-kfdtest"
INSTALL_DIR="${ROCM_PATH:?ROCM_PATH must be set}"

CMAKE_PREFIX_PATH="$INSTALL_DIR;$INSTALL_DIR/lib/cmake;$INSTALL_DIR/llvm;$INSTALL_DIR/llvm/lib/cmake"

export PKG_CONFIG_PATH="$INSTALL_DIR/lib/pkgconfig:$INSTALL_DIR/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

COMMON_CMAKE_ARGS=(
  -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR"
  -DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH"
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
  -DCMAKE_C_COMPILER_LAUNCHER=ccache
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
)

cmake -G Ninja -S "$SRC_DIR" -B "$BUILD_DIR" \
  "${COMMON_CMAKE_ARGS[@]}"
cmake --build "$BUILD_DIR"
cmake --install "$BUILD_DIR"

"$SCRIPT_DIR/sync_rocm_sdk_links.py" core "$INSTALL_DIR" \
  libhsa-runtime64.so

cmake -G Ninja -S "$SRC_DIR/rocrtst/suites/test_common" -B "$ROCRTST_BUILD_DIR" \
  "${COMMON_CMAKE_ARGS[@]}" \
  -DROCM_SYSTEMS_ROOT="$SCRIPT_DIR" \
  -DTARGET_DEVICES=gfx1151 \
  -DROCRTST_BLD_TYPE=Release
cmake --build "$ROCRTST_BUILD_DIR"

LIBHSAKMT_PATH="$INSTALL_DIR/lib" \
cmake -G Ninja -S "$SRC_DIR/libhsakmt/tests/kfdtest" -B "$KFDTEST_BUILD_DIR" \
  "${COMMON_CMAKE_ARGS[@]}"
cmake --build "$KFDTEST_BUILD_DIR"

echo "ROCr build complete. Artifacts installed in $INSTALL_DIR"
echo "ROCr tests built:"
echo "  $ROCRTST_BUILD_DIR/rocrtst64"
echo "  $KFDTEST_BUILD_DIR/kfdtest"
