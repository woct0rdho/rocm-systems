#!/usr/bin/env python3
"""Run thread tracing test and collect diagnostics."""

import os
import shutil
import sys
from datetime import datetime
from pathlib import Path

from test_pc_sampling import (
    BIN,
    LOG_ROOT,
    PCS_TIMEOUT_KILL_AFTER_SEC,
    PCS_TIMEOUT_SEC,
    ROC_PROF,
    WORKLOAD_ENV,
    check_stale_processes,
    cleanup_sampling_procs,
    collect_dmesg_since,
    extract_summary,
    read_uptime,
    run_profiler_with_timeout,
    write_config_log,
    write_ldd_log,
    write_proc_snapshot,
    write_sampling_proc_snapshot,
)

OUT_DIR = Path(__file__).resolve().parent / "thread_trace_test_out"


def env_flag(name: str) -> bool:
    value = os.environ.get(name, "")
    return value.lower() in {"1", "true", "yes", "on"}


def find_stats_csv(out_dir: Path) -> Path | None:
    """Find the most recent stats csv file."""
    candidates = sorted(
        out_dir.rglob("stats*.csv"),
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    return candidates[0] if candidates else None


def find_att_file(out_dir: Path) -> Path | None:
    """Find the most recent .att file."""
    candidates = sorted(
        out_dir.rglob("*.att"),
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    return candidates[0] if candidates else None


def find_occupancy_json(out_dir: Path) -> Path | None:
    """Find the most recent occupancy.json file."""
    candidates = sorted(
        out_dir.rglob("occupancy.json"),
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    return candidates[0] if candidates else None


def main() -> int:
    run_tag = "thread_trace_" + datetime.now().strftime("%Y%m%d_%H%M%S")
    run_dir = LOG_ROOT / run_tag
    run_dir.mkdir(parents=True, exist_ok=True)
    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    start_uptime = read_uptime()

    print(f"run_tag={run_tag}")
    write_config_log(run_dir)
    write_ldd_log(run_dir)
    write_proc_snapshot(run_dir, "start")
    write_sampling_proc_snapshot(run_dir, "start")

    if check_stale_processes():
        print("FAILED: stale rocprofv3/pc_sampling_test process detected before run.")
        return 2

    env = {**os.environ, **WORKLOAD_ENV}
    att_no_detail = env_flag("ROCPROF_ATT_PARAM_NO_DETAIL")

    # Override number of launches since ATT only traces the first kernel
    # instance by default, there is no need to launch it 2000 times
    env["PCS_KERNEL_LAUNCHES"] = "1"

    cmd = [
        str(ROC_PROF),
        "--att",
        "-d",
        str(OUT_DIR),
        "--",
        str(BIN),
    ]

    print(f"\nRunning: {' '.join(cmd)}")
    print(f"Timeout: {PCS_TIMEOUT_SEC}s (kill after +{PCS_TIMEOUT_KILL_AFTER_SEC}s)\n")

    run_log_path = run_dir / "rocprof_run.log"
    run_status = run_profiler_with_timeout(cmd, env, run_log_path)

    (run_dir / "dmesg_since_start.log").write_text(collect_dmesg_since(start_uptime))
    write_proc_snapshot(run_dir, "end")
    write_sampling_proc_snapshot(run_dir, "end")
    extract_summary(run_dir)

    if run_status != 0:
        cleanup_sampling_procs(str(BIN))
        if run_status == 124:
            print(f"FAILED: rocprofv3 timed out after {PCS_TIMEOUT_SEC}s")
        else:
            print(f"FAILED: rocprofv3 exited with status {run_status}")
        print(f"Logs: {run_dir}")
        return run_status

    if check_stale_processes():
        print("FAILED: stale rocprofv3/pc_sampling_test process detected after run.")
        cleanup_sampling_procs(str(BIN))
        print(f"Logs: {run_dir}")
        return 3

    csv_file = find_stats_csv(OUT_DIR)
    att_file = find_att_file(OUT_DIR)
    occupancy_file = find_occupancy_json(OUT_DIR)

    if csv_file is None and not att_no_detail:
        print(f"FAILED: no stats*.csv produced in {OUT_DIR}")
        print(f"Logs: {run_dir}")
        return 1

    if att_file is None:
        print(f"FAILED: no .att file produced in {OUT_DIR}")
        print(f"Logs: {run_dir}")
        return 1

    line_count = sum(1 for _ in csv_file.open()) if csv_file is not None else 0
    if att_no_detail:
        if occupancy_file is None:
            print(f"FAILED: no occupancy.json produced in {OUT_DIR}")
            print(f"Logs: {run_dir}")
            return 1
        print(
            f"\nThread Trace OK (no-detail): {occupancy_file}, {att_file.name} generated"
        )
        print(f"Logs: {run_dir}")
        return 0

    if line_count <= 1:
        print(f"FAILED: stats*.csv has no samples ({csv_file})")
        print(f"Logs: {run_dir}")
        return 1

    print(
        f"\nThread Trace OK: {csv_file} ({line_count} lines), {att_file.name} generated"
    )
    print(f"Logs: {run_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
