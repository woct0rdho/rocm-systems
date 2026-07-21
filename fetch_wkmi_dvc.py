from __future__ import annotations

import hashlib
import os
import re
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent
POINTER = ROOT / "shared/amdgpu-windows-interop/wkmi/win/lib/wkmi.lib.dvc"
TARGET = POINTER.with_suffix("")
REMOTE_CONFIG = ROOT / ".dvc/config"


def parse_pointer(path: Path) -> tuple[str, int]:
    text = path.read_text(encoding="utf-8")
    md5_match = re.search(r"^\s*-?\s*md5:\s*([0-9a-fA-F]{32})\s*$", text, re.MULTILINE)
    size_match = re.search(r"^\s*size:\s*(\d+)\s*$", text, re.MULTILINE)
    if not md5_match or not size_match:
        raise RuntimeError(f"Could not parse DVC pointer: {path}")
    return md5_match.group(1).lower(), int(size_match.group(1))


def parse_s3_remote(path: Path) -> tuple[str, str]:
    text = path.read_text(encoding="utf-8")
    url_match = re.search(r"^\s*url\s*=\s*s3://([^/\s]+)(?:/([^\s]+))?\s*$", text, re.MULTILINE)
    anonymous_match = re.search(
        r"^\s*allow_anonymous_login\s*=\s*true\s*$", text, re.MULTILINE | re.IGNORECASE
    )
    if not url_match or not anonymous_match:
        raise RuntimeError(f"No anonymous S3 DVC remote found in: {path}")
    return url_match.group(1), (url_match.group(2) or "").strip("/")


def verify(path: Path, expected_md5: str, expected_size: int) -> tuple[int, str]:
    digest = hashlib.md5()
    size = 0
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
            size += len(chunk)
    actual_md5 = digest.hexdigest()
    if size != expected_size:
        raise RuntimeError(f"Size mismatch for {path}: expected {expected_size}, got {size}")
    if actual_md5 != expected_md5:
        raise RuntimeError(f"MD5 mismatch for {path}: expected {expected_md5}, got {actual_md5}")
    return size, actual_md5


def main() -> int:
    expected_md5, expected_size = parse_pointer(POINTER)
    bucket, prefix = parse_s3_remote(REMOTE_CONFIG)

    if TARGET.exists():
        size, actual_md5 = verify(TARGET, expected_md5, expected_size)
        print(f"wkmi_status=already_present path={TARGET} size={size} md5={actual_md5}")
        return 0

    key_parts = [part for part in (prefix, "files", "md5", expected_md5[:2], expected_md5[2:]) if part]
    key = "/".join(key_parts)
    url = f"https://{bucket}.s3.amazonaws.com/{key}"
    temporary = TARGET.with_name(TARGET.name + ".download")
    temporary.unlink(missing_ok=True)

    request = urllib.request.Request(url, headers={"User-Agent": "rocm-systems-wkmi-fetch/1"})
    try:
        with urllib.request.urlopen(request, timeout=60) as response, temporary.open("wb") as output:
            while chunk := response.read(1024 * 1024):
                output.write(chunk)
        size, actual_md5 = verify(temporary, expected_md5, expected_size)
        os.replace(temporary, TARGET)
    except Exception:
        temporary.unlink(missing_ok=True)
        raise

    print(f"wkmi_status=fetched path={TARGET} size={size} md5={actual_md5}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"wkmi_status=failed error={error}", file=sys.stderr)
        raise
