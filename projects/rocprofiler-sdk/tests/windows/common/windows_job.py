from __future__ import annotations

import sys
from pathlib import Path


PRODUCTION_BIN = Path(__file__).resolve().parents[3] / "source" / "bin"
if str(PRODUCTION_BIN) not in sys.path:
    sys.path.insert(0, str(PRODUCTION_BIN))

from _rocprofv3_windows_job import SuspendedWindowsJob, WindowsJobError, run_in_job


__all__ = ("SuspendedWindowsJob", "WindowsJobError", "run_in_job")
