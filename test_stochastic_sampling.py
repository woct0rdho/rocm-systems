#!/usr/bin/env python3
"""Run PC sampling stochastic test and collect diagnostics."""

import csv
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

OUT_DIR = Path(__file__).resolve().parent / "stochastic_test_out"
PCS_INTERVAL = "1048576"  # powers of 2 for cycles


def find_stochastic_csv(out_dir: Path) -> Path | None:
    """Find the most recent pc_sampling_stochastic csv file."""
    candidates = sorted(
        out_dir.rglob("*pc_sampling_stochastic*.csv"),
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    return candidates[0] if candidates else None


def find_agent_info_csv(out_dir: Path) -> Path | None:
    candidates = sorted(
        out_dir.rglob("*agent_info*.csv"),
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    return candidates[0] if candidates else None


def has_gfx1151_agent(out_dir: Path) -> bool:
    agent_info = find_agent_info_csv(out_dir)
    if agent_info is None:
        return False

    with agent_info.open() as f:
        return any(row.get("Name") == "gfx1151" for row in csv.DictReader(f))


def validate_decoded_metadata(csv_file: Path, allow_zero_wave_count: bool) -> tuple[bool, str]:
    with csv_file.open() as f:
        rows = list(csv.DictReader(f))

    if not rows:
        return False, f"FAILED: pc_sampling_stochastic csv has no samples ({csv_file})"

    def is_issued(value: str) -> bool:
        return value.lower() in {"1", "true"}

    issued_rows = [row for row in rows if is_issued(row["Wave_Issued_Instruction"])]
    non_default_inst = [
        row
        for row in rows
        if row["Instruction_Type"] != "ROCPROFILER_PC_SAMPLING_INSTRUCTION_TYPE_NONE"
    ]
    non_default_reason = [
        row
        for row in rows
        if row["Stall_Reason"]
        != "ROCPROFILER_PC_SAMPLING_INSTRUCTION_NOT_ISSUED_REASON_NONE"
    ]
    non_zero_wave_count = [row for row in rows if int(row["Wave_Count"]) > 0]

    if not issued_rows:
        return False, "FAILED: no issued stochastic samples were decoded"

    if not non_default_inst:
        return False, "FAILED: all stochastic instruction types remained at NONE"

    if not non_default_reason:
        return False, "FAILED: all stochastic stall reasons remained at NONE"

    if not non_zero_wave_count and not allow_zero_wave_count:
        return False, "FAILED: all stochastic wave counts remained zero"

    wave_count_summary = "all_zero"
    if non_zero_wave_count:
        wave_count_summary = (
            f"{min(int(row['Wave_Count']) for row in non_zero_wave_count)}-"
            f"{max(int(row['Wave_Count']) for row in non_zero_wave_count)}"
        )

    return True, (
        f"decoded rows={len(rows)}, issued={len(issued_rows)}, "
        f"non_default_inst={len(non_default_inst)}, "
        f"non_default_reason={len(non_default_reason)}, "
        f"wave_count_range={wave_count_summary}"
    )


def main() -> int:
    run_tag = "stochastic_" + datetime.now().strftime("%Y%m%d_%H%M%S")
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
    cmd = [
        str(ROC_PROF),
        "--pc-sampling-beta-enabled",
        "1",
        "--pc-sampling-method",
        "stochastic",
        "--pc-sampling-unit",
        "cycles",
        "--pc-sampling-interval",
        PCS_INTERVAL,
        "--output-format",
        "csv",
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

    csv_file = find_stochastic_csv(OUT_DIR)
    if csv_file is None:
        print(f"FAILED: no pc_sampling_stochastic csv produced in {OUT_DIR}")
        print(f"Logs: {run_dir}")
        return 1

    valid, summary = validate_decoded_metadata(csv_file, has_gfx1151_agent(OUT_DIR))
    if not valid:
        print(summary)
        print(f"Logs: {run_dir}")
        return 1

    line_count = sum(1 for _ in csv_file.open())
    print(f"\nPC sampling stochastic OK: {csv_file} ({line_count} lines)")
    print(summary)
    print(f"Logs: {run_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
