#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/projects/rocprof-trace-decoder"
BUILD_DIR="$SCRIPT_DIR/build/rocprof-trace-decoder"
INSTALL_DIR="$ROCM_PATH"

cmake -G Ninja -S "$SRC_DIR" -B "$BUILD_DIR" \
  -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DBUILD_TESTS=ON \
  -DBUILD_UNIT_TESTS=ON \
  -DBUILD_PYTHON=OFF \
  -DDISABLE_COMGR=ON
cmake --build "$BUILD_DIR"
cmake --install "$BUILD_DIR"

"$SCRIPT_DIR/sync_rocm_sdk_links.py" core "$INSTALL_DIR" \
  librocprof-trace-decoder.so

# rocprofiler-sdk links the decoder at build time and resolves it through the
# SDK library directory at run time. Other ROCm SDK packages ship their own copy
# in a sibling directory; such a copy would shadow the freshly built decoder and
# silently degrade gfx11 quick scan, so repoint those entries at this install.
DEVEL_LIB_DIR="$(cd "$INSTALL_DIR/lib" && pwd)"
for sibling in core libraries; do
  shadow_dir="$INSTALL_DIR/../_rocm_sdk_${sibling}/lib"
  [[ -d "$shadow_dir" ]] || continue
  for entry in "$shadow_dir"/librocprof-trace-decoder.so*; do
    [[ -e "$entry" || -L "$entry" ]] || continue
    target="$(basename "$entry")"
    if [[ -L "$entry" && "$(readlink -f "$entry")" == "$DEVEL_LIB_DIR/$target" ]]; then
      continue
    fi
    rm -f "$entry"
    ln -s "$(realpath --relative-to="$shadow_dir" "$DEVEL_LIB_DIR")/$target" "$entry"
    echo "Re-linked $target in $shadow_dir"
  done
done

echo "Trace decoder build complete. Artifacts installed in $INSTALL_DIR"
