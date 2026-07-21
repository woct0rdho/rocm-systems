from __future__ import annotations

import os
from pathlib import Path
import sys
import time

from windows_job import run_in_job


def clean_environment():
    environment = dict(os.environ)
    for name in (
        "HSA_TOOLS_LIB",
        "ROCR_USE_PM4",
        "WSLKMT_VENDOR_PACKET",
    ):
        environment.pop(name, None)
    return environment


def test_normal_exit_and_output(tmp_path):
    output = tmp_path / "normal.txt"
    result = run_in_job(
        [sys.executable, "-c", "print('job_normal=passed'); raise SystemExit(23)"],
        clean_environment(),
        tmp_path,
        10.0,
        output,
    )
    assert result["exit_code"] == 23
    assert not result["timed_out"]
    assert result["duration_ns"] > 0
    assert output.read_text(encoding="utf-8").strip() == "job_normal=passed"


def test_timeout_terminates_root(tmp_path):
    output = tmp_path / "timeout.txt"
    start = time.monotonic()
    result = run_in_job(
        [sys.executable, "-c", "import time; time.sleep(30)"],
        clean_environment(),
        tmp_path,
        0.2,
        output,
    )
    assert result["timed_out"]
    assert result["exit_code"] == 124
    assert time.monotonic() - start < 5.0


def test_timeout_terminates_process_tree(tmp_path):
    child_marker = tmp_path / "child-survived.txt"
    child_code = (
        "import pathlib,time; "
        "time.sleep(1.5); "
        f"pathlib.Path({str(child_marker)!r}).write_text('survived', encoding='utf-8')"
    )
    parent_code = (
        "import subprocess,sys,time; "
        f"subprocess.Popen([sys.executable, '-c', {child_code!r}]); "
        "time.sleep(30)"
    )
    result = run_in_job(
        [sys.executable, "-c", parent_code],
        clean_environment(),
        tmp_path,
        0.4,
        tmp_path / "tree.txt",
    )
    assert result["timed_out"]
    time.sleep(2.0)
    assert not child_marker.exists()
