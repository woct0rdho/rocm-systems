#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$ROOT_DIR/pc_sampling_test"

CLANGXX="$ROCM_PATH/lib/llvm/bin/clang++"

# Build the test binary
"$CLANGXX" \
  -x hip \
  --offload-arch=gfx1151 \
  --rocm-path="$ROCM_PATH" \
  -I"$ROCM_PATH/include" \
  -L"$ROCM_PATH/lib" \
  -D__HIP_PLATFORM_AMD__ \
  -O3 \
  -std=c++17 \
  -lamdhip64 \
  "$ROOT_DIR/pc_sampling_test.cpp" \
  -o "$BIN"
