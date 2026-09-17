#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/test/hip_late_dso"
BUILD_DIR="$SCRIPT_DIR/build/hip-late-dso-test"
INSTALL_DIR="${ROCM_PATH:?ROCM_PATH must be set}"
CXX="$INSTALL_DIR/llvm/bin/amdclang++"

if [ ! -x "$CXX" ]; then
  CXX="$INSTALL_DIR/lib/llvm/bin/amdclang++"
fi
if [ ! -x "$CXX" ]; then
  echo "Unable to find amdclang++ under $INSTALL_DIR" >&2
  exit 1
fi

mkdir -p "$BUILD_DIR"

COMMON_FLAGS=(
  -x hip
  --rocm-path="$INSTALL_DIR"
  --offload-arch=gfx1151
  -I"$INSTALL_DIR/include"
  -O2
  -g
)

"$CXX" "${COMMON_FLAGS[@]}" -fPIC -shared \
  "$SRC_DIR/late_dso_plugin.cpp" \
  -Wl,-soname,libhip_late_dso_plugin.so \
  -Wl,-rpath,"$INSTALL_DIR/lib" \
  -o "$BUILD_DIR/libhip_late_dso_plugin.so"

"$CXX" "${COMMON_FLAGS[@]}" \
  "$SRC_DIR/late_dso_host.cpp" \
  -ldl \
  -Wl,-rpath,"$INSTALL_DIR/lib" \
  -o "$BUILD_DIR/hip_late_dso_host"

printf 'Late-DSO test fixture built in %s\n' "$BUILD_DIR"
readelf -d "$BUILD_DIR/libhip_late_dso_plugin.so" | grep -E 'NEEDED|SONAME|RUNPATH|RPATH' || true
