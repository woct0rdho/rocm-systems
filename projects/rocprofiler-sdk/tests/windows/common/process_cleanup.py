from __future__ import annotations

import csv
import os
from pathlib import Path
import shutil
import subprocess
import time


def target_processes(target: Path) -> tuple[list[list[str]], list[str]]:
    target = target.resolve()
    image = target.name
    tasklist = Path(os.environ["SystemRoot"]) / "System32" / "tasklist.exe"
    tasklist_result = subprocess.run(
        [str(tasklist), "/fi", f"imagename eq {image}", "/fo", "csv", "/nh"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=15,
    )
    tasklist_rows = []
    if tasklist_result.returncode == 0:
        tasklist_rows = [
            row
            for row in csv.reader(tasklist_result.stdout.splitlines())
            if row and row[0].casefold() == image.casefold()
        ]

    powershell = shutil.which("pwsh.exe") or shutil.which("powershell.exe")
    if not powershell:
        raise RuntimeError("PowerShell is required for the Win32_Process cleanup check")
    escaped_image = image.replace("'", "''")
    escaped_target = str(target).replace("'", "''")
    wmi_result = subprocess.run(
        [
            powershell,
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            (
                f"$expected = [IO.Path]::GetFullPath('{escaped_target}'); "
                f"Get-CimInstance Win32_Process -Filter \"Name = '{escaped_image}'\" | "
                "Where-Object { $_.ExecutablePath -and "
                "[IO.Path]::GetFullPath($_.ExecutablePath).Equals($expected, "
                "[StringComparison]::OrdinalIgnoreCase) } | "
                "ForEach-Object { '{0}`t{1}' -f $_.ProcessId, $_.ExecutablePath }"
            ),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=20,
    )
    if wmi_result.returncode != 0:
        raise RuntimeError(f"Win32_Process query failed:\n{wmi_result.stdout}")
    return tasklist_rows, [line for line in wmi_result.stdout.splitlines() if line.strip()]


def assert_target_stopped(target: Path) -> None:
    observed = ([], [])
    for _ in range(10):
        observed = target_processes(target)
        if not observed[0] and not observed[1]:
            return
        time.sleep(0.1)
    raise RuntimeError(
        f"target process remained after rocprofv3 returned: "
        f"tasklist={observed[0]} Win32_Process={observed[1]}"
    )
