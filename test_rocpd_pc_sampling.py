#!/usr/bin/env python3
"""Run PC sampling host-trap with RocPD output and validate the rocpd_pc_sampling table.

Exercises the RocPD output path added on top of the PC-sampling support:
the run must produce a database whose rocpd_pc_sampling view holds the
host-trap samples, with the method discriminator enforced, the stochastic-only
columns left null for host-trap rows, and an intact database.
"""

import os
import shutil
import sqlite3
import subprocess
import sys
from datetime import datetime
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent
OUT_DIR = ROOT_DIR / "rocpd_pc_sampling_test_out"
LOG_ROOT = ROOT_DIR / "logs"
BIN = ROOT_DIR / "pc_sampling_test"

ROCM_PATH = os.environ.get("ROCM_PATH", "/opt/rocm")
ROC_PROF = Path(ROCM_PATH) / "bin" / "rocprofv3"

PCS_INTERVAL_US = os.environ.get("PCS_INTERVAL_US", "5000")
PCS_TIMEOUT_SEC = int(os.environ.get("PCS_TIMEOUT_SEC", "120"))

# Schema version that introduced rocpd_pc_sampling.
MIN_SCHEMA_VERSION = (3, 0, 5)

STOCHASTIC_ONLY_COLUMNS = (
    "wave_issued_instruction",
    "instruction_type",
    "stall_reason",
    "wave_count",
)


def fail(message: str, run_dir: Path | None = None) -> int:
    print(f"FAILED: {message}")
    if run_dir is not None:
        print(f"Logs: {run_dir}")
    return 1


def find_database(root: Path) -> Path | None:
    dbs = sorted(root.glob("**/*.db"))
    return dbs[0] if dbs else None


def parse_schema_version(text: str) -> tuple[int, int, int] | None:
    try:
        parts = [int(part) for part in str(text).split(".")]
    except ValueError:
        return None
    while len(parts) < 3:
        parts.append(0)
    return tuple(parts[:3])


def validate_database(db_path: Path) -> tuple[bool, str]:
    con = sqlite3.connect(db_path)
    try:
        cur = con.cursor()

        row = cur.execute(
            "SELECT value FROM rocpd_metadata WHERE tag = 'schema_version' LIMIT 1").fetchone()
        if row is None:
            return False, "rocpd_metadata has no schema_version entry"
        version = parse_schema_version(row[0])
        if version is None or version < MIN_SCHEMA_VERSION:
            return False, f"schema version {row[0]!r} is older than {MIN_SCHEMA_VERSION}"
        print(f"schema_version={row[0]}")

        # Reading through the view also proves the data view exists.
        try:
            by_method = cur.execute(
                "SELECT method, COUNT(*) FROM rocpd_pc_sampling GROUP BY method").fetchall()
        except sqlite3.Error as exc:
            return False, f"rocpd_pc_sampling is not queryable: {exc}"
        counts = dict(by_method)
        print(f"rocpd_pc_sampling rows by method: {counts}")
        if counts.get("host_trap", 0) == 0:
            return False, "no host_trap rows in rocpd_pc_sampling"

        unknown = [m for m in counts if m not in ("host_trap", "stochastic")]
        if unknown:
            return False, f"unexpected method values: {unknown}"

        # The method CHECK constraint is part of the schema contract. It lives on
        # the base table; rocpd_pc_sampling is the data view over it.
        base_table = cur.execute(
            "SELECT name FROM sqlite_master WHERE type = 'table'"
            " AND name LIKE 'rocpd_pc_sampling_%' LIMIT 1").fetchone()
        if base_table is None:
            return False, "no rocpd_pc_sampling_* base table"
        print(f"base table: {base_table[0]}")
        con.execute("SAVEPOINT method_constraint")
        try:
            cur.execute(
                f"INSERT INTO `{base_table[0]}`"
                "(method, timestamp, exec_mask, dispatch_id, correlation_id)"
                " VALUES ('bogus', 0, 0, 0, 0)")
        except sqlite3.IntegrityError:
            print("method constraint rejects unknown values: ok")
        else:
            return False, "method CHECK constraint did not reject an unknown method"
        finally:
            con.execute("ROLLBACK TO method_constraint")

        non_null = cur.execute(
            "SELECT COUNT(*) FROM rocpd_pc_sampling"
            " WHERE method = 'host_trap' AND ("
            + " OR ".join(f"{col} IS NOT NULL" for col in STOCHASTIC_ONLY_COLUMNS)
            + ")").fetchone()[0]
        print(f"host_trap rows with non-null stochastic columns: {non_null}")
        if non_null:
            return False, "host-trap rows must leave the stochastic-only columns null"

        joined = cur.execute(
            "SELECT COUNT(*) FROM rocpd_pc_sampling AS s"
            " JOIN rocpd_kernel_dispatch AS d ON d.dispatch_id = s.dispatch_id").fetchone()[0]
        print(f"host_trap rows joining a kernel dispatch: {joined}")
        if joined == 0:
            return False, "no PC-sampling row joins a kernel dispatch record"

        integrity = cur.execute("PRAGMA integrity_check").fetchone()[0]
        violations = len(cur.execute("PRAGMA foreign_key_check").fetchall())
        print(f"integrity_check={integrity}, foreign_key_violations={violations}")
        if integrity != "ok":
            return False, f"integrity_check reported {integrity!r}"
        if violations:
            return False, f"{violations} foreign key violations"
    finally:
        con.close()
    return True, "ok"


def main() -> int:
    if not ROC_PROF.exists():
        return fail(f"rocprofv3 not found at {ROC_PROF}")
    if not BIN.exists():
        return fail(f"{BIN} not found; run ./build_pc_sampling_test.sh first")

    stale = subprocess.run(["pgrep", "-af", "rocprofv3|pc_sampling_test"],
                           capture_output=True, text=True)
    stale_lines = [l for l in stale.stdout.splitlines() if str(os.getpid()) not in l]
    if stale_lines:
        return fail("stale rocprofv3/pc_sampling_test process detected before run")

    run_tag = f"rocpd_pc_sampling_{datetime.now():%Y%m%d_%H%M%S}"
    run_dir = LOG_ROOT / run_tag
    run_dir.mkdir(parents=True, exist_ok=True)
    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)

    cmd = [
        str(ROC_PROF),
        "--pc-sampling-beta-enabled", "1",
        "--pc-sampling-method", "host_trap",
        "--pc-sampling-unit", "time",
        "--pc-sampling-interval", PCS_INTERVAL_US,
        "--kernel-trace",
        "--output-format", "rocpd",
        "-d", str(OUT_DIR),
        "--", str(BIN),
    ]
    env = dict(os.environ, HSA_DISABLE_XDNA="1")

    print(f"\nRunning: {' '.join(cmd)}")
    print(f"Timeout: {PCS_TIMEOUT_SEC}s\n")
    try:
        proc = subprocess.run(cmd, env=env, capture_output=True, text=True,
                              timeout=PCS_TIMEOUT_SEC)
    except subprocess.TimeoutExpired:
        return fail(f"rocprofv3 timed out after {PCS_TIMEOUT_SEC}s", run_dir)

    (run_dir / "rocprof_run.log").write_text(
        f"$ {' '.join(cmd)}\n\n[stdout]\n{proc.stdout}\n\n[stderr]\n{proc.stderr}\n")

    stale = subprocess.run(["pgrep", "-af", "rocprofv3|pc_sampling_test"],
                           capture_output=True, text=True)
    stale_lines = [l for l in stale.stdout.splitlines() if str(os.getpid()) not in l]
    if stale_lines:
        return fail("stale rocprofv3/pc_sampling_test process detected after run", run_dir)

    if proc.returncode != 0:
        print(proc.stdout[-2000:])
        print(proc.stderr[-2000:])
        return fail(f"rocprofv3 exited with status {proc.returncode}", run_dir)

    db_path = find_database(OUT_DIR)
    if db_path is None:
        return fail(f"no .db produced under {OUT_DIR}", run_dir)
    print(f"database: {db_path}")

    ok, message = validate_database(db_path)
    if not ok:
        return fail(message, run_dir)

    print(f"\nRocPD PC sampling OK: {db_path}")
    print(f"Logs: {run_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
