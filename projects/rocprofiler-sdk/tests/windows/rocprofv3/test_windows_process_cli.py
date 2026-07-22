from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

import pytest


def run_cli(*arguments: str):
    python = Path(os.environ["ROCPROFV3_TEST_PYTHON"]).resolve()
    script = Path(os.environ["ROCPROFV3_TEST_BUILT_SCRIPT"]).resolve()
    env = dict(os.environ)
    for name in (
        "HSA_TOOLS_LIB",
        "ROCR_USE_PM4",
        "WSLKMT_VENDOR_PACKET",
    ):
        env.pop(name, None)
    return subprocess.run(
        [str(python), str(script), *arguments],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=30,
    )


def test_help_and_version():
    help_result = run_cli("--help")
    assert help_result.returncode == 0
    assert "ROCProfilerV3 Run Script" in help_result.stdout
    assert "--list-avail" in help_result.stdout

    version_result = run_cli("--version")
    assert version_result.returncode == 0
    assert re.search(r"^\s*version:\s+1\.3\.5\s*$", version_result.stdout, re.MULTILINE)
    assert re.search(
        r"^\s*git_revision:\s+[0-9a-f]{40}\s*$",
        version_result.stdout,
        re.MULTILINE,
    )
    assert re.search(
        r"^\s*system_name:\s+Windows\s*$", version_result.stdout, re.MULTILINE
    )


def test_unsupported_target_is_rejected_before_launch(tmp_path):
    marker = tmp_path / "target-launched.txt"
    target = Path(os.environ["SystemRoot"]) / "System32" / "cmd.exe"
    result = run_cli("--", str(target), "/c", f"echo launched>{marker}")
    assert result.returncode == 1
    assert "Target application profiling requires --kernel-trace" in result.stdout
    assert not marker.exists()


@pytest.mark.parametrize("selector", ("--kernel-trace", "--hip-runtime-trace"))
def test_target_status_is_preserved_without_trace(selector):
    target = Path(os.environ["SystemRoot"]) / "System32" / "cmd.exe"
    result = run_cli(selector, "--", str(target), "/c", "exit", "37")
    assert result.returncode == 37
    assert "Fatal error" not in result.stdout


@pytest.mark.parametrize(
    ("selector", "output_name"),
    (
        ("--kernel-trace", "retained_kernel_trace.csv"),
        ("--hip-runtime-trace", "retained_hip_api_trace.csv"),
    ),
)
def test_existing_output_is_rejected_before_launch(tmp_path, selector, output_name):
    output = tmp_path / output_name
    output.write_text("retained", encoding="utf-8")
    marker = tmp_path / "target-launched.txt"
    target = Path(os.environ["SystemRoot"]) / "System32" / "cmd.exe"
    result = run_cli(
        selector,
        "--output-directory",
        str(tmp_path),
        "--output-file",
        "retained",
        "--",
        str(target),
        "/c",
        f"echo launched>{marker}",
    )
    assert result.returncode == 1
    assert "output already exists" in result.stdout
    assert output.read_text(encoding="utf-8") == "retained"
    assert not marker.exists()


def test_existing_kernel_stats_output_is_rejected_before_launch(tmp_path):
    output = tmp_path / "retained_kernel_stats.csv"
    output.write_text("retained", encoding="utf-8")
    marker = tmp_path / "target-launched.txt"
    target = Path(os.environ["SystemRoot"]) / "System32" / "cmd.exe"
    result = run_cli(
        "--kernel-trace",
        "--stats",
        "--output-directory",
        str(tmp_path),
        "--output-file",
        "retained",
        "--",
        str(target),
        "/c",
        f"echo launched>{marker}",
    )
    assert result.returncode == 1
    assert "output already exists" in result.stdout
    assert output.read_text(encoding="utf-8") == "retained"
    assert not (tmp_path / "retained_kernel_trace.csv").exists()
    assert not marker.exists()
