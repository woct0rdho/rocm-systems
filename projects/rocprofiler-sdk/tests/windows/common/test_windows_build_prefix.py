from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess


def test_alternate_prefix_is_its_own_dependency_root(tmp_path):
    repository = Path(os.environ["ROCPROFILER_TEST_REPOSITORY_ROOT"]).resolve()
    common = repository / "tools" / "windows-build" / "common.ps1"
    active = (tmp_path / "active" / "_rocm_sdk_devel").resolve()
    alternate = (tmp_path / "comparison").resolve()
    active.mkdir(parents=True)
    alternate.mkdir(parents=True)

    pwsh = shutil.which("pwsh.exe") or shutil.which("powershell.exe")
    assert pwsh, "PowerShell is required"
    command = (
        f". '{common}'; "
        f"Get-WindowsDependencyRoot -InstallPrefix '{alternate}' "
        f"-ActiveDevelRoot '{active}'; "
        f"Get-WindowsDependencyRoot -InstallPrefix '{active}' "
        f"-ActiveDevelRoot '{active}'"
    )
    result = subprocess.run(
        [pwsh, "-NoLogo", "-NoProfile", "-NonInteractive", "-Command", command],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=30,
    )
    assert result.returncode == 0, result.stdout
    paths = [Path(line.strip()).resolve() for line in result.stdout.splitlines() if line.strip()]
    assert paths == [alternate, active]


def test_alternate_prefix_mismatch_fails_without_mutating_active_package(tmp_path):
    repository = Path(os.environ["ROCPROFILER_TEST_REPOSITORY_ROOT"]).resolve()
    active = Path(os.environ["ROCM_PATH"]).resolve()
    sentinel = active / "bin" / "rocprofiler-sdk.dll"
    assert sentinel.is_file()
    before = hashlib.sha256(sentinel.read_bytes()).hexdigest()

    alternate = (tmp_path / "comparison").resolve()
    alternate.mkdir()
    pwsh = shutil.which("pwsh.exe") or shutil.which("powershell.exe")
    assert pwsh, "PowerShell is required"
    result = subprocess.run(
        [
            pwsh,
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-File",
            str(repository / "projects/rocprofiler-sdk/scripts/build_windows.ps1"),
            "-VenvPath",
            os.environ["VIRTUAL_ENV"],
            "-InstallPrefix",
            str(alternate),
            "-BuildDirectory",
            str(tmp_path / "build"),
            "-ConfigureOnly",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=30,
    )
    assert result.returncode != 0
    plain_output = re.sub(r"\x1b\[[0-9;]*m", "", result.stdout)
    assert "The Windows runtime installation is incomplete:" in plain_output
    assert "comparison\\bin\\amdhip64_7.dll" in re.sub(r"\s+", "", plain_output)
    assert hashlib.sha256(sentinel.read_bytes()).hexdigest() == before
    assert not list(alternate.rglob("*"))


def test_component_scripts_use_explicit_dependency_roots():
    repository = Path(os.environ["ROCPROFILER_TEST_REPOSITORY_ROOT"]).resolve()
    scripts = {
        "aqlprofile": repository / "projects/aqlprofile/scripts/build_windows.ps1",
        "clr": repository / "projects/clr/scripts/build_windows_runtime.ps1",
        "sdk": repository / "projects/rocprofiler-sdk/scripts/build_windows.ps1",
    }
    contents = {name: path.read_text(encoding="utf-8") for name, path in scripts.items()}
    assert all("Get-WindowsDependencyRoot" in text for text in contents.values())
    assert "[string]$HsaIncludeDirectory" in contents["aqlprofile"]
    assert '"-DAQLPROFILE_HSA_INCLUDE_DIR=$HsaIncludeDirectory"' in contents[
        "aqlprofile"
    ]
    assert '"-DROCM_PATH=$dependencyRoot"' in contents["clr"]
    assert '"-DROCM_PATH=$RuntimeRoot"' in contents["sdk"]
    assert '"-DROCPROFILER_WINDOWS_HSA_INCLUDE_DIR=$(Join-Path $RuntimeRoot' in contents[
        "sdk"
    ]
