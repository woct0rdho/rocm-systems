#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/projects/clr"
BUILD_DIR="$SCRIPT_DIR/build/clr-hip"
INSTALL_DIR="${ROCM_PATH:?ROCM_PATH must be set}"
HIP_COMMON_DIR="$SCRIPT_DIR/projects/hip"
LLVM_BIN="$INSTALL_DIR/llvm/bin"
OPENGL_ROOT="$SCRIPT_DIR/build/sysdeps/opengl/usr"
OPENGL_LIB_DIR="$OPENGL_ROOT/lib/x86_64-linux-gnu"

if [ ! -x "$LLVM_BIN/amdclang" ] || [ ! -x "$LLVM_BIN/amdclang++" ]; then
  LLVM_BIN="$INSTALL_DIR/lib/llvm/bin"
fi

if [ ! -x "$LLVM_BIN/amdclang" ] || [ ! -x "$LLVM_BIN/amdclang++" ]; then
  echo "Unable to find amdclang/amdclang++ under $INSTALL_DIR" >&2
  exit 1
fi

if [ ! -f "$OPENGL_ROOT/include/GL/gl.h" ] || [ ! -e "$OPENGL_LIB_DIR/libOpenGL.so" ] || [ ! -e "$OPENGL_LIB_DIR/libGLX.so" ]; then
  echo "Local OpenGL development files are missing under $OPENGL_ROOT" >&2
  echo "Extract the binary libgl/libglx/libopengl/libegl development packages there first." >&2
  exit 1
fi

CMAKE_PREFIX_PATH="$INSTALL_DIR;$INSTALL_DIR/lib/cmake;$INSTALL_DIR/llvm;$INSTALL_DIR/llvm/lib/cmake;$INSTALL_DIR/lib/llvm;$INSTALL_DIR/lib/llvm/lib/cmake;$OPENGL_ROOT"

export PATH="$INSTALL_DIR/bin:$LLVM_BIN:$PATH"
export PKG_CONFIG_PATH="$INSTALL_DIR/lib/pkgconfig:$INSTALL_DIR/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

cmake -G Ninja -S "$SRC_DIR" -B "$BUILD_DIR" \
  -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
  -DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH" \
  -DROCM_PATH="$INSTALL_DIR" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER="$LLVM_BIN/amdclang" \
  -DCMAKE_CXX_COMPILER="$LLVM_BIN/amdclang++" \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_C_FLAGS="-I$OPENGL_ROOT/include" \
  -DCMAKE_CXX_FLAGS="-I$OPENGL_ROOT/include" \
  -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=ON \
  -DOPENGL_INCLUDE_DIR="$OPENGL_ROOT/include" \
  -DOPENGL_opengl_LIBRARY="$OPENGL_LIB_DIR/libOpenGL.so" \
  -DOPENGL_glx_LIBRARY="$OPENGL_LIB_DIR/libGLX.so" \
  -DCLR_BUILD_HIP=ON \
  -DCLR_BUILD_OCL=OFF \
  -DHIP_COMMON_DIR="$HIP_COMMON_DIR" \
  -DHIPCC_BIN_DIR="$INSTALL_DIR/bin" \
  -DHIP_PLATFORM=amd \
  -DBUILD_SHARED_LIBS=ON \
  -DROCCLR_ENABLE_HSA=ON \
  -DROCCLR_ENABLE_PAL=OFF \
  -DROCR_DLL_LOAD=OFF \
  -D__HIP_ENABLE_PCH=OFF \
  -D__HIP_ENABLE_RTC=ON \
  -DROCM_KPACK_ENABLED=ON \
  -DUSE_PROF_API=OFF \
  -DHIP_ENABLE_ROCPROFILER_REGISTER=ON

cmake --build "$BUILD_DIR"
cmake --install "$BUILD_DIR"

"$SCRIPT_DIR/sync_rocm_sdk_links.py" core "$INSTALL_DIR" \
  libamdhip64.so \
  libhiprtc.so \
  libhiprtc-builtins.so

printf 'HIP runtime build complete. Artifacts installed in %s\n' "$INSTALL_DIR"
printf 'Installed libamdhip64: '
readlink -f "$INSTALL_DIR/lib/libamdhip64.so.7"
