#!/bin/bash
set -euo pipefail

# Define installation path
# INSTALL_DIR=$(pwd)/install
INSTALL_DIR="$ROCM_PATH"
mkdir -p "$INSTALL_DIR"

# ROCm sysdeps path (for libdw/libelf on some systems)
SYSDEPS_PATH=""
if [[ -d "$ROCM_PATH/lib/rocm_sysdeps" ]]; then
  SYSDEPS_PATH="$ROCM_PATH/lib/rocm_sysdeps"
fi

# Build ROCProfiler
echo "Building ROCProfiler..."
mkdir -p build/rocprofiler-sdk
cd build/rocprofiler-sdk
cmake ../../projects/rocprofiler-sdk \
  -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
  -DCMAKE_PREFIX_PATH="$INSTALL_DIR;$ROCM_PATH;$SYSDEPS_PATH" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DGPU_TARGETS=gfx1151 \
  -DAMDGPU_TARGETS=gfx1151
make -j"$(nproc)"
make install
cd ../..

echo "ROCProfiler build complete. Artifacts installed in $INSTALL_DIR"
