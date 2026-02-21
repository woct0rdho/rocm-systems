#!/bin/bash
set -euo pipefail

# Define installation path
# INSTALL_DIR=$(pwd)/install
INSTALL_DIR="$ROCM_PATH"
mkdir -p "$INSTALL_DIR"

# Build ROCr
echo "Building ROCr..."
mkdir -p build/rocr-runtime
cd build/rocr-runtime
cmake ../../projects/rocr-runtime \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DGPU_TARGETS=gfx1151 \
    -DAMDGPU_TARGETS=gfx1151
make -j"$(nproc)"
make install
cd ../..

echo "ROCr build complete. Artifacts installed in $INSTALL_DIR"
