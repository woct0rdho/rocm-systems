#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build/hip-late-dso-test"
PLUGIN="$BUILD_DIR/libhip_late_dso_plugin.so"
HOST="$BUILD_DIR/hip_late_dso_host"
LOG_DIR="$SCRIPT_DIR/logs/hip-late-dso"
INSTALL_DIR="${ROCM_PATH:?ROCM_PATH must be set}"
PYTHON_BIN="${PYTHON_BIN:-$HOME/venv_torch/bin/python}"

mkdir -p "$LOG_DIR"

check_after_crash() {
  local label="$1"
  {
    echo "===== crash follow-up: $label ====="
    date --iso-8601=seconds
    echo "--- possibly stale related processes ---"
    pgrep -af 'python|hip_late_dso|ModuleTest|rocprof|hrr-playback' || true
    echo "--- recent kernel log ---"
    dmesg --ctime 2>&1 | tail -80 || true
  } | tee "$LOG_DIR/${label}.crash-check.log"
}

run_case() {
  local label="$1"
  shift
  local log="$LOG_DIR/${label}.log"

  echo "===== $label ====="
  set +e
  timeout --signal=TERM --kill-after=10s 120s \
    env HSA_DISABLE_XDNA=1 \
        LD_LIBRARY_PATH="$INSTALL_DIR/lib:$INSTALL_DIR/lib/rocm_sysdeps/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        "$@" > >(tee "$log") 2>&1
  local status=$?
  set -e
  echo "exit status: $status" | tee -a "$log"

  if [ "$status" -ne 0 ]; then
    check_after_crash "$label"
    return "$status"
  fi
}

if [ ! -x "$HOST" ] || [ ! -f "$PLUGIN" ]; then
  "$SCRIPT_DIR/build_hip_late_dso_test.sh"
fi

case "${1:-}" in
  before-init|unused-exit|used-exit|used-dlclose|used-pending-dlclose)
    run_case "$1" "$HOST" "$PLUGIN" "$1"
    ;;
  repeat-dlclose)
    iterations="${2:-100}"
    run_case "repeat-dlclose-${iterations}" "$HOST" "$PLUGIN" repeat-dlclose "$iterations"
    ;;
  torch-unused)
    run_case torch-unused "$PYTHON_BIN" "$SCRIPT_DIR/test/hip_late_dso/torch_late_dso.py" "$PLUGIN"
    ;;
  torch-used)
    run_case torch-used "$PYTHON_BIN" "$SCRIPT_DIR/test/hip_late_dso/torch_late_dso.py" "$PLUGIN" --use-plugin
    ;;
  bitsandbytes)
    run_case bitsandbytes "$PYTHON_BIN" -c \
      'import torch, bitsandbytes; torch.randn(64, device="cuda"); print("bitsandbytes case completed before process finalization", flush=True)'
    ;;
  unsloth)
    run_case unsloth "$PYTHON_BIN" -c \
      'import unsloth; print("Unsloth case completed before process finalization", flush=True)'
    ;;
  late-import-order)
    run_case late-import-order "$PYTHON_BIN" -c \
      'import torch; import peft; import unsloth; print("late-import-order case completed before process finalization", flush=True)'
    ;;
  full-late-import-order)
    run_case full-late-import-order "$PYTHON_BIN" -c \
      'import torch; import transformers; import peft; import trl; import unsloth; import unsloth_zoo; print("full-late-import-order case completed before process finalization", flush=True)'
    ;;
  *)
    cat >&2 <<EOF
usage: $0 <case> [iterations]

cases:
  before-init
  unused-exit
  used-exit
  used-dlclose
  used-pending-dlclose
  repeat-dlclose [iterations]
  torch-unused
  torch-used
  bitsandbytes
  unsloth
  late-import-order
  full-late-import-order
EOF
    exit 2
    ;;
esac
