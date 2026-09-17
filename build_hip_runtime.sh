#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/projects/clr"
BUILD_DIR="$SCRIPT_DIR/build/clr-hip"
INSTALL_DIR="${ROCM_PATH:?ROCM_PATH must be set}"
HIP_COMMON_DIR="$SCRIPT_DIR/projects/hip"
LLVM_BIN="$INSTALL_DIR/llvm/bin"
OPENGL_ROOT="$SCRIPT_DIR/build/sysdeps/opengl/usr"
OPENGL_SYSTEM_INCLUDE_DIR="/usr/include"
OPENGL_SYSTEM_LIB_DIR="/usr/lib/x86_64-linux-gnu"

if [ ! -x "$LLVM_BIN/amdclang" ] || [ ! -x "$LLVM_BIN/amdclang++" ]; then
  LLVM_BIN="$INSTALL_DIR/lib/llvm/bin"
fi

if [ ! -x "$LLVM_BIN/amdclang" ] || [ ! -x "$LLVM_BIN/amdclang++" ]; then
  echo "Unable to find amdclang/amdclang++ under $INSTALL_DIR" >&2
  exit 1
fi

# OpenGL: prefer the distribution development packages, and fall back to a
# tree extracted under build/sysdeps/opengl for systems without them.
if [ -f "$OPENGL_SYSTEM_INCLUDE_DIR/GL/gl.h" ] &&
   [ -f "$OPENGL_SYSTEM_INCLUDE_DIR/GL/glext.h" ] &&
   [ -e "$OPENGL_SYSTEM_LIB_DIR/libOpenGL.so" ] &&
   [ -e "$OPENGL_SYSTEM_LIB_DIR/libGLX.so" ]; then
  OPENGL_INCLUDE_DIR="$OPENGL_SYSTEM_INCLUDE_DIR"
  OPENGL_LIB_DIR="$OPENGL_SYSTEM_LIB_DIR"
  OPENGL_PREFIX=""
  echo "Using the system OpenGL development files"
elif [ -f "$OPENGL_ROOT/include/GL/gl.h" ] &&
     [ -f "$OPENGL_ROOT/include/GL/glext.h" ] &&
     [ -e "$OPENGL_ROOT/lib/x86_64-linux-gnu/libOpenGL.so" ] &&
     [ -e "$OPENGL_ROOT/lib/x86_64-linux-gnu/libGLX.so" ]; then
  OPENGL_INCLUDE_DIR="$OPENGL_ROOT/include"
  OPENGL_LIB_DIR="$OPENGL_ROOT/lib/x86_64-linux-gnu"
  OPENGL_PREFIX="$OPENGL_ROOT"
  echo "Using the OpenGL development files extracted under $OPENGL_ROOT"
else
  cat >&2 <<EOF
OpenGL development files are missing.
Install them with:
  sudo apt-get install libgl-dev libglx-dev libopengl-dev libegl-dev
or extract the binary libgl/libglx/libopengl/libegl development packages under
  $OPENGL_ROOT
EOF
  exit 1
fi

OPENGL_CMAKE_ARGS=(
  -DOPENGL_INCLUDE_DIR="$OPENGL_INCLUDE_DIR"
  -DOPENGL_opengl_LIBRARY="$OPENGL_LIB_DIR/libOpenGL.so"
  -DOPENGL_glx_LIBRARY="$OPENGL_LIB_DIR/libGLX.so"
)
if [ -e "$OPENGL_LIB_DIR/libGL.so" ]; then
  OPENGL_CMAKE_ARGS+=(-DOPENGL_gl_LIBRARY="$OPENGL_LIB_DIR/libGL.so")
fi
if [ -e "$OPENGL_LIB_DIR/libEGL.so" ]; then
  OPENGL_CMAKE_ARGS+=(-DOPENGL_egl_LIBRARY="$OPENGL_LIB_DIR/libEGL.so")
fi

# CLR links ${OPENGL_LIBRARIES} but does not add the include directory to its
# targets, so pass it for every translation unit.
OPENGL_CFLAGS="-I$OPENGL_INCLUDE_DIR"

CMAKE_PREFIX_PATH="$INSTALL_DIR;$INSTALL_DIR/lib/cmake;$INSTALL_DIR/llvm;$INSTALL_DIR/llvm/lib/cmake;$INSTALL_DIR/lib/llvm;$INSTALL_DIR/lib/llvm/lib/cmake"
if [ -n "$OPENGL_PREFIX" ]; then
  CMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH;$OPENGL_PREFIX"
fi

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
  -DCMAKE_C_FLAGS="$OPENGL_CFLAGS" \
  -DCMAKE_CXX_FLAGS="$OPENGL_CFLAGS" \
  -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=ON \
  "${OPENGL_CMAKE_ARGS[@]}" \
  -DCLR_BUILD_HIP=ON \
  -DCLR_BUILD_OCL=OFF \
  -DHIP_COMMON_DIR="$HIP_COMMON_DIR" \
  -DHIPCC_BIN_DIR="$INSTALL_DIR/bin" \
  -DHIP_PLATFORM=amd \
  -DBUILD_SHARED_LIBS=ON \
  -DROCCLR_ENABLE_HSA=ON \
  -DROCCLR_ENABLE_PAL=OFF \
  -DROCR_DLL_LOAD=OFF \
  -D__HIP_ENABLE_PCH=ON \
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
