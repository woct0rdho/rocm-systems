#!/usr/bin/env python3

import argparse
import os
import shutil
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path


@dataclass(frozen=True)
class Mode:
    destination_env: str
    destination_dirname: str
    synchronized_description: str
    already_synchronized_description: str


MODES = {
    "core": Mode(
        destination_env="ROCM_SDK_CORE_DIR",
        destination_dirname="_rocm_sdk_core",
        synchronized_description="core library",
        already_synchronized_description="Core library",
    ),
    "libraries": Mode(
        destination_env="ROCM_SDK_LIBRARIES_DIR",
        destination_dirname="_rocm_sdk_libraries",
        synchronized_description="libraries runtime",
        already_synchronized_description="Libraries runtime",
    ),
}


class BackupStore:
    def __init__(self, parent: Path, prefix: str) -> None:
        self.parent = parent
        self.prefix = prefix
        self.path: Path | None = None

    def save(self, entry: Path, relative_entry: Path) -> None:
        if self.path is None:
            timestamp = datetime.now().strftime("%Y%m%d%H%M%S%f")
            self.path = self.parent / f"{self.prefix}{timestamp}"
            self.path.mkdir(parents=True)

        destination = self.path / relative_entry
        destination.parent.mkdir(parents=True, exist_ok=True)
        if entry.is_symlink():
            destination.symlink_to(os.readlink(entry))
        elif entry.is_dir():
            shutil.copytree(entry, destination, symlinks=True)
        else:
            shutil.copy2(entry, destination)


def warn(message: str) -> None:
    print(f"sync warning: {message}", file=sys.stderr)


def entry_exists(entry: Path) -> bool:
    return entry.exists() or entry.is_symlink()


def remove_entry(entry: Path) -> None:
    if entry.is_symlink() or entry.is_file():
        entry.unlink()
    elif entry.is_dir():
        shutil.rmtree(entry)


def validate_relative_entry(value: str) -> Path:
    entry = Path(value)
    if entry.is_absolute() or not entry.parts or any(part == ".." for part in entry.parts):
        raise argparse.ArgumentTypeError(f"entry must stay below the devel lib directory: {value}")
    return entry


def resolved_same_file(left: Path, right: Path) -> bool:
    try:
        return left.resolve(strict=True) == right.resolve(strict=True) or os.path.samefile(left, right)
    except (FileNotFoundError, OSError):
        return False


def normalize_version_aliases(
    devel_lib_dir: Path, requested_entries: list[Path], backups: BackupStore
) -> int:
    normalized = 0

    for relative_request in requested_entries:
        unversioned_entry = devel_lib_dir / relative_request
        if unversioned_entry.is_dir() or not unversioned_entry.is_symlink():
            continue

        raw_soname_target = Path(os.readlink(unversioned_entry))
        soname_entry = (
            raw_soname_target
            if raw_soname_target.is_absolute()
            else unversioned_entry.parent / raw_soname_target
        )
        if soname_entry.parent.resolve() != unversioned_entry.parent.resolve():
            warn(f"major-version link is outside the requested library directory: {unversioned_entry}")
            continue

        prefix = f"{unversioned_entry.name}."
        if not soname_entry.name.startswith(prefix):
            warn(f"unable to identify the major-version link for {unversioned_entry}")
            continue

        major = soname_entry.name[len(prefix) :]
        if not major.isdigit() or not soname_entry.is_file():
            warn(f"major-version link does not resolve to a file: {soname_entry}")
            continue

        candidate_prefix = f"{unversioned_entry.name}.{major}."
        for candidate in sorted(unversioned_entry.parent.iterdir()):
            if not candidate.name.startswith(candidate_prefix) or not entry_exists(candidate):
                continue
            if candidate.is_dir() and not candidate.is_symlink():
                warn(f"refusing to replace version-like directory: {candidate}")
                continue
            if resolved_same_file(candidate, soname_entry):
                continue

            relative_candidate = candidate.relative_to(devel_lib_dir)
            backups.save(candidate, relative_candidate)
            remove_entry(candidate)
            candidate.symlink_to(os.path.relpath(soname_entry, candidate.parent))
            normalized += 1

    return normalized


def matching_entries(devel_lib_dir: Path, relative_request: Path) -> list[Path]:
    requested_entry = devel_lib_dir / relative_request
    if requested_entry.is_dir():
        return [requested_entry]
    if not requested_entry.parent.is_dir():
        return []
    return sorted(
        entry
        for entry in requested_entry.parent.iterdir()
        if entry.name.startswith(requested_entry.name) and entry_exists(entry)
    )


def synchronize_entries(
    devel_lib_dir: Path,
    destination_lib_dir: Path,
    requested_entries: list[Path],
    backups: BackupStore,
) -> int:
    synchronized = 0
    handled: set[Path] = set()

    for relative_request in requested_entries:
        entries = matching_entries(devel_lib_dir, relative_request)
        if not entries:
            warn(f"no devel entries match {devel_lib_dir / relative_request}*")
            continue

        for devel_entry in entries:
            if devel_entry in handled:
                continue
            handled.add(devel_entry)

            relative_entry = devel_entry.relative_to(devel_lib_dir)
            destination_entry = destination_lib_dir / relative_entry
            destination_entry.parent.mkdir(parents=True, exist_ok=True)
            desired_target = os.path.relpath(devel_entry, destination_entry.parent)

            if destination_entry.is_symlink() and os.readlink(destination_entry) == desired_target:
                continue

            if entry_exists(destination_entry):
                backups.save(destination_entry, relative_entry)
                remove_entry(destination_entry)

            destination_entry.symlink_to(desired_target, target_is_directory=devel_entry.is_dir())
            synchronized += 1

    return synchronized


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=MODES)
    parser.add_argument("devel_prefix", type=Path)
    parser.add_argument("entries", nargs="+", type=validate_relative_entry)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    mode = MODES[args.mode]
    devel_lib_dir = args.devel_prefix / "lib"
    destination_root = Path(
        os.environ.get(
            mode.destination_env,
            str(args.devel_prefix.parent / mode.destination_dirname),
        )
    )
    destination_lib_dir = destination_root / "lib"

    if not devel_lib_dir.is_dir():
        print(f"sync skipped: devel lib dir does not exist: {devel_lib_dir}", file=sys.stderr)
        return 0
    if not destination_lib_dir.is_dir():
        print(
            f"sync skipped: destination lib dir does not exist: {destination_lib_dir}",
            file=sys.stderr,
        )
        return 0
    if devel_lib_dir.resolve() == destination_lib_dir.resolve():
        print(
            f"sync skipped: devel and destination lib dirs are the same: {devel_lib_dir}",
            file=sys.stderr,
        )
        return 0

    devel_backups = BackupStore(devel_lib_dir, ".pre-current-version-alias-")
    destination_backups = BackupStore(destination_lib_dir, ".pre-devel-symlink-")

    normalized = normalize_version_aliases(devel_lib_dir, args.entries, devel_backups)
    synchronized = synchronize_entries(
        devel_lib_dir, destination_lib_dir, args.entries, destination_backups
    )

    if normalized:
        print(
            f"Normalized {normalized} stale devel version alias(es) "
            "to the current major-version link"
        )
        print(f"Previous devel entries backed up in {devel_backups.path}")

    if synchronized:
        print(
            f"Synchronized {synchronized} {mode.synchronized_description} link(s) "
            f"to {devel_lib_dir}"
        )
        if destination_backups.path is not None:
            print(f"Previous destination entries backed up in {destination_backups.path}")
    else:
        print(
            f"{mode.already_synchronized_description} links already synchronized "
            f"to {devel_lib_dir}"
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
