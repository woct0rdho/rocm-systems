#!/usr/bin/env python3
"""Run PC sampling host-trap test and collect diagnostics."""

import os
import re
import shutil
import signal
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent
OUT_DIR = ROOT_DIR / "pc_sampling_test_out"
LOG_ROOT = ROOT_DIR / "logs"
BIN = ROOT_DIR / "pc_sampling_test"

ROCM_PATH = os.environ.get("ROCM_PATH", "/opt/rocm")
ROC_PROF = Path(ROCM_PATH) / "bin" / "rocprofv3"

PCS_INTERVAL_US = os.environ.get("PCS_INTERVAL_US", "5000")
PCS_TIMEOUT_SEC = int(os.environ.get("PCS_TIMEOUT_SEC", "15"))
PCS_TIMEOUT_KILL_AFTER_SEC = int(os.environ.get("PCS_TIMEOUT_KILL_AFTER_SEC", "15"))

# Workload parameters (passed to pc_sampling_test via env)
WORKLOAD_ENV = {
    "PCS_N": os.environ.get("PCS_N", "1048576"),
    "PCS_KERNEL_LAUNCHES": os.environ.get("PCS_KERNEL_LAUNCHES", "2000"),
    "PCS_INNER_ITERS": os.environ.get("PCS_INNER_ITERS", "128"),
}

# Module parameters to check
MODULE_PARAMS = [
    "amdkfd_gfx11_pcs_sqcmd_policy",
    "amdkfd_gfx11_pcs_max_injected_traps",
    "amdkfd_gfx11_pcs_post_status_delay_us",
]


def read_uptime() -> float:
    return float(Path("/proc/uptime").read_text().split()[0])


def run_cmd(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    """Run a command, returning CompletedProcess. Never raises on failure."""
    return subprocess.run(cmd, capture_output=True, text=True, **kwargs)


def collect_dmesg_since(start_uptime: float) -> str:
    """Return dmesg lines with timestamp >= start_uptime."""
    result = run_cmd(["dmesg"])
    if result.returncode != 0:
        return f"(dmesg failed: {result.stderr.strip()})\n"
    lines = []
    for line in result.stdout.splitlines():
        m = re.match(r"^\[\s*(\d+\.\d+)\]", line)
        if m and float(m.group(1)) >= start_uptime:
            lines.append(line)
        elif not m:
            lines.append(line)
    return "\n".join(lines) + "\n"


def cleanup_lingering_procs(bin_name: str) -> None:
    """SIGTERM then SIGKILL any lingering processes matching bin_name."""
    result = run_cmd(["pgrep", "-f", bin_name])
    pids = result.stdout.split()
    if not pids:
        return
    print(f"cleanup: terminating lingering {bin_name} pids: {' '.join(pids)}")
    for pid in pids:
        try:
            os.kill(int(pid), signal.SIGTERM)
        except OSError:
            pass
    time.sleep(1)
    result = run_cmd(["pgrep", "-f", bin_name])
    pids = result.stdout.split()
    for pid in pids:
        print(f"cleanup: force killing {bin_name} pid {pid}")
        try:
            os.kill(int(pid), signal.SIGKILL)
        except OSError:
            pass


def check_stale_processes() -> bool:
    """Return True if stale rocprofv3/pc_sampling_test processes exist."""
    for name in ["rocprofv3", "pc_sampling_test"]:
        result = run_cmd(["pgrep", "-x", name])
        if result.returncode == 0 and result.stdout.strip():
            return True
    return False


def read_module_param(name: str) -> str:
    p = Path(f"/sys/module/amdgpu/parameters/{name}")
    if p.exists():
        return p.read_text().strip()
    return "<unavailable>"


def write_config_log(run_dir: Path) -> None:
    """Write config.log and print config to stdout."""
    lines = [
        f"rocprof={ROC_PROF}",
        f"bin={BIN}",
        f"out_dir={OUT_DIR}",
        f"pcs_interval_us={PCS_INTERVAL_US}",
        f"PCS_TIMEOUT_SEC={PCS_TIMEOUT_SEC}",
        f"PCS_TIMEOUT_KILL_AFTER_SEC={PCS_TIMEOUT_KILL_AFTER_SEC}",
        f"uname={run_cmd(['uname', '-r']).stdout.strip()}",
    ]
    for k, v in WORKLOAD_ENV.items():
        lines.append(f"{k}={v}")

    modinfo = run_cmd(["modinfo", "-n", "amdgpu"])
    lines.append(f"amdgpu_module={modinfo.stdout.strip()}")

    for param in MODULE_PARAMS:
        lines.append(f"{param}={read_module_param(param)}")

    text = "\n".join(lines) + "\n"
    (run_dir / "config.log").write_text(text)
    print(text, end="")


def write_ldd_log(run_dir: Path) -> None:
    parts = []
    for label, path in [("rocprofv3", ROC_PROF), ("pc_sampling_test", BIN)]:
        result = run_cmd(["ldd", str(path)])
        parts.append(f"[ldd {label}]\n{result.stdout}\n")
    (run_dir / "ldd.log").write_text("\n".join(parts))


def write_proc_snapshot(run_dir: Path, suffix: str) -> None:
    result = run_cmd(["ps", "-eo", "pid,ppid,stat,etime,comm,args"])
    (run_dir / f"proc_{suffix}.log").write_text(result.stdout)


def list_sampling_processes() -> list[str]:
    result = run_cmd(["ps", "-eo", "pid,ppid,stat,etime,comm,args"])
    return [
        line
        for line in result.stdout.splitlines()
        if "rocprofv3" in line or "pc_sampling_test" in line
    ]


def write_sampling_proc_snapshot(run_dir: Path, suffix: str) -> None:
    lines = list_sampling_processes()
    text = "\n".join(lines)
    if text:
        text += "\n"
    (run_dir / f"sampling_procs_{suffix}.log").write_text(text)


def cleanup_sampling_procs(bin_name: str) -> None:
    cleanup_lingering_procs(bin_name)
    cleanup_lingering_procs("rocprofv3")


def run_profiler_with_timeout(
    cmd: list[str], env: dict[str, str], run_log_path: Path
) -> int:
    with run_log_path.open("w") as log_file:
        proc = subprocess.Popen(
            cmd,
            env=env,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            proc.wait(timeout=PCS_TIMEOUT_SEC)
            return proc.returncode
        except subprocess.TimeoutExpired:
            log_file.write(f"\n(timeout after {PCS_TIMEOUT_SEC}s; sending SIGTERM)\n")
            proc.terminate()
            try:
                proc.wait(timeout=PCS_TIMEOUT_KILL_AFTER_SEC)
            except subprocess.TimeoutExpired:
                log_file.write(
                    f"(still running after {PCS_TIMEOUT_KILL_AFTER_SEC}s grace; sending SIGKILL)\n"
                )
                proc.kill()
                proc.wait()
            return 124


def extract_summary(run_dir: Path) -> None:
    """Extract interesting markers from rocprof and dmesg logs."""
    lines = []

    lines.append("[dmesg markers]")
    dmesg_path = run_dir / "dmesg_since_start.log"
    if dmesg_path.exists():
        dmesg_patterns = re.compile(
            r"program_trap_handler_settings_v11|pcs hosttrap|pcs:|"
            r"trigger_pc_sample_trap|page fault|MES failed|GPU reset|VM fault|FAULT"
        )
        for line in dmesg_path.read_text().splitlines():
            if dmesg_patterns.search(line):
                lines.append(line)
    else:
        lines.append("dmesg_since_start.log missing")

    (run_dir / "summary.log").write_text("\n".join(lines) + "\n")


def find_csv(out_dir: Path) -> Path | None:
    """Find the most recent pc_sampling_host_trap CSV file."""
    candidates = sorted(
        out_dir.rglob("*pc_sampling_host_trap*.csv"),
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    return candidates[0] if candidates else None


def main() -> int:
    # Setup directories
    run_tag = datetime.now().strftime("%Y%m%d_%H%M%S")
    run_dir = LOG_ROOT / run_tag
    run_dir.mkdir(parents=True, exist_ok=True)
    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    start_uptime = read_uptime()

    # Write config
    print(f"run_tag={run_tag}")
    write_config_log(run_dir)
    write_ldd_log(run_dir)
    write_proc_snapshot(run_dir, "start")
    write_sampling_proc_snapshot(run_dir, "start")

    # Check for stale processes
    if check_stale_processes():
        print("FAILED: stale rocprofv3/pc_sampling_test process detected before run.")
        result = run_cmd(["ps", "-eo", "pid,ppid,stat,etime,comm,args"])
        for line in result.stdout.splitlines():
            if "rocprofv3" in line or "pc_sampling_test" in line:
                print(line)
        print(f"Logs: {run_dir}")
        return 2

    # Run rocprofv3 with PC sampling
    env = {**os.environ, **WORKLOAD_ENV}
    cmd = [
        str(ROC_PROF),
        "--pc-sampling-beta-enabled",
        "1",
        "--pc-sampling-method",
        "host_trap",
        "--pc-sampling-unit",
        "time",
        "--pc-sampling-interval",
        PCS_INTERVAL_US,
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

    # Collect post-run diagnostics
    (run_dir / "dmesg_since_start.log").write_text(collect_dmesg_since(start_uptime))
    write_proc_snapshot(run_dir, "end")
    write_sampling_proc_snapshot(run_dir, "end")
    extract_summary(run_dir)

    # Check result
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

    csv_file = find_csv(OUT_DIR)
    if csv_file is None:
        print(f"FAILED: no pc_sampling_host_trap csv produced in {OUT_DIR}")
        print(f"Logs: {run_dir}")
        return 1

    line_count = sum(1 for _ in csv_file.open())
    if line_count <= 1:
        print(f"FAILED: pc_sampling_host_trap csv has no samples ({csv_file})")
        print(f"Logs: {run_dir}")
        return 1

    print(f"\nPC sampling host-trap OK: {csv_file} ({line_count} lines)")
    print(f"Logs: {run_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
