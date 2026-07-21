from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import sys


def owned_path(path: Path, owner_root: Path) -> Path:
    path = path.resolve()
    owner_root = owner_root.resolve()
    if path == owner_root:
        raise ValueError("pytest basetemp must not equal its owner root")
    try:
        path.relative_to(owner_root)
    except ValueError as error:
        raise ValueError(f"pytest basetemp is outside its owner root: {path}") from error
    return path


def remove_tree(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run pytest with a build-owned temporary root and remove it afterward"
    )
    parser.add_argument("--owner-root", type=Path, required=True)
    parser.add_argument("--basetemp", type=Path, required=True)
    parser.add_argument("pytest_arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    basetemp = owned_path(args.basetemp, args.owner_root)
    pytest_arguments = list(args.pytest_arguments)
    if pytest_arguments[:1] == ["--"]:
        pytest_arguments.pop(0)
    if not pytest_arguments:
        parser.error("at least one pytest argument is required")

    remove_tree(basetemp)
    command = [
        sys.executable,
        "-m",
        "pytest",
        "-s",
        "--tb=short",
        "--basetemp",
        str(basetemp),
        *pytest_arguments,
    ]
    try:
        return subprocess.run(command, check=False).returncode
    finally:
        remove_tree(basetemp)


if __name__ == "__main__":
    raise SystemExit(main())
