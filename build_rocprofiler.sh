#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/projects/rocprofiler-sdk"
BUILD_DIR="$SCRIPT_DIR/build/rocprofiler-sdk"
INSTALL_DIR="$ROCM_PATH"

cmake -G Ninja -S "$SRC_DIR" -B "$BUILD_DIR" \
  -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER="$ROCM_PATH/llvm/bin/amdclang" \
  -DCMAKE_CXX_COMPILER="$ROCM_PATH/llvm/bin/amdclang++" \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_EXE_LINKER_FLAGS="-L$ROCM_PATH/llvm/lib" \
  -DCMAKE_SHARED_LINKER_FLAGS="-L$ROCM_PATH/llvm/lib" \
  -DGPU_TARGETS=gfx1151 \
  -DAMDGPU_TARGETS=gfx1151 \
  -DOPENMP_GPU_TARGETS=gfx1151 \
  -DROCPROFILER_BUILD_TESTS=ON \
  -DROCPROFILER_BUILD_SAMPLES=ON \
  -DROCPSDK_PYTEST_EXECUTABLE="pytest"
cmake --build "$BUILD_DIR"
cmake --install "$BUILD_DIR"

"$SCRIPT_DIR/sync_rocm_sdk_links.py" core "$INSTALL_DIR" \
  librocprofiler-sdk.so \
  librocprofiler-sdk-roctx.so \
  librocprofiler-sdk-rocpd.so \
  librocprofiler-sdk-rocattach.so \
  librocprofiler-sdk-attach.so

echo "ROCProfiler build complete. Artifacts installed in $INSTALL_DIR"
