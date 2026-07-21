# MIT License
#
# Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

from __future__ import annotations

import ctypes
from ctypes import wintypes
import msvcrt
import os
from pathlib import Path
import subprocess
import time


ULONG_PTR = ctypes.c_size_t
SIZE_T = ctypes.c_size_t


class WindowsJobError(RuntimeError):
    pass


class IO_COUNTERS(ctypes.Structure):
    _fields_ = [
        ("ReadOperationCount", ctypes.c_uint64),
        ("WriteOperationCount", ctypes.c_uint64),
        ("OtherOperationCount", ctypes.c_uint64),
        ("ReadTransferCount", ctypes.c_uint64),
        ("WriteTransferCount", ctypes.c_uint64),
        ("OtherTransferCount", ctypes.c_uint64),
    ]


class JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("PerProcessUserTimeLimit", ctypes.c_int64),
        ("PerJobUserTimeLimit", ctypes.c_int64),
        ("LimitFlags", wintypes.DWORD),
        ("MinimumWorkingSetSize", SIZE_T),
        ("MaximumWorkingSetSize", SIZE_T),
        ("ActiveProcessLimit", wintypes.DWORD),
        ("Affinity", ULONG_PTR),
        ("PriorityClass", wintypes.DWORD),
        ("SchedulingClass", wintypes.DWORD),
    ]


class JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("BasicLimitInformation", JOBOBJECT_BASIC_LIMIT_INFORMATION),
        ("IoInfo", IO_COUNTERS),
        ("ProcessMemoryLimit", SIZE_T),
        ("JobMemoryLimit", SIZE_T),
        ("PeakProcessMemoryUsed", SIZE_T),
        ("PeakJobMemoryUsed", SIZE_T),
    ]


class STARTUPINFOW(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD),
        ("lpReserved", wintypes.LPWSTR),
        ("lpDesktop", wintypes.LPWSTR),
        ("lpTitle", wintypes.LPWSTR),
        ("dwX", wintypes.DWORD),
        ("dwY", wintypes.DWORD),
        ("dwXSize", wintypes.DWORD),
        ("dwYSize", wintypes.DWORD),
        ("dwXCountChars", wintypes.DWORD),
        ("dwYCountChars", wintypes.DWORD),
        ("dwFillAttribute", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("wShowWindow", wintypes.WORD),
        ("cbReserved2", wintypes.WORD),
        ("lpReserved2", ctypes.POINTER(ctypes.c_ubyte)),
        ("hStdInput", wintypes.HANDLE),
        ("hStdOutput", wintypes.HANDLE),
        ("hStdError", wintypes.HANDLE),
    ]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("hProcess", wintypes.HANDLE),
        ("hThread", wintypes.HANDLE),
        ("dwProcessId", wintypes.DWORD),
        ("dwThreadId", wintypes.DWORD),
    ]


def _win32_error(operation: str) -> WindowsJobError:
    code = ctypes.get_last_error()
    return WindowsJobError(f"{operation} failed with Win32 error {code}")


def _kernel32():
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    kernel32.CreateJobObjectW.restype = wintypes.HANDLE
    kernel32.SetInformationJobObject.argtypes = [
        wintypes.HANDLE,
        ctypes.c_int,
        ctypes.c_void_p,
        wintypes.DWORD,
    ]
    kernel32.SetInformationJobObject.restype = wintypes.BOOL
    kernel32.CreateProcessW.argtypes = [
        wintypes.LPCWSTR,
        wintypes.LPWSTR,
        ctypes.c_void_p,
        ctypes.c_void_p,
        wintypes.BOOL,
        wintypes.DWORD,
        ctypes.c_void_p,
        wintypes.LPCWSTR,
        ctypes.POINTER(STARTUPINFOW),
        ctypes.POINTER(PROCESS_INFORMATION),
    ]
    kernel32.CreateProcessW.restype = wintypes.BOOL
    kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    kernel32.AssignProcessToJobObject.restype = wintypes.BOOL
    kernel32.ResumeThread.argtypes = [wintypes.HANDLE]
    kernel32.ResumeThread.restype = wintypes.DWORD
    kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel32.WaitForSingleObject.restype = wintypes.DWORD
    kernel32.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
    kernel32.TerminateJobObject.restype = wintypes.BOOL
    kernel32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    kernel32.TerminateProcess.restype = wintypes.BOOL
    kernel32.GetExitCodeProcess.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(wintypes.DWORD),
    ]
    kernel32.GetExitCodeProcess.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL
    kernel32.GetStdHandle.argtypes = [wintypes.DWORD]
    kernel32.GetStdHandle.restype = wintypes.HANDLE
    return kernel32


class SuspendedWindowsJob:
    """Create a suspended child in a kill-on-close Windows job object."""

    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000
    JOB_OBJECT_EXTENDED_LIMIT_INFORMATION = 9
    CREATE_SUSPENDED = 0x00000004
    CREATE_UNICODE_ENVIRONMENT = 0x00000400
    STARTF_USESTDHANDLES = 0x00000100
    STD_INPUT_HANDLE = ctypes.c_uint32(-10).value
    WAIT_OBJECT_0 = 0
    WAIT_TIMEOUT = 258
    INFINITE = 0xFFFFFFFF
    TERMINATION_WAIT_MS = 5000
    STILL_ACTIVE = 259

    def __init__(
        self,
        command: list[str],
        environment: dict[str, str],
        cwd: Path,
        stdout_path: Path | None = None,
        *,
        error_exit_code: int = 125,
    ):
        if os.name != "nt":
            raise WindowsJobError("the job-object launcher requires native Windows")
        if not command:
            raise WindowsJobError("the job-object launcher requires a command")

        self.command = [str(value) for value in command]
        self.environment = {str(key): str(value) for key, value in environment.items()}
        self.cwd = Path(cwd).resolve()
        self.stdout_path = Path(stdout_path).resolve() if stdout_path else None
        self.error_exit_code = error_exit_code
        if not self.cwd.is_dir():
            raise WindowsJobError(
                f"the job-object working directory does not exist: {self.cwd}"
            )
        if self.stdout_path is not None and self.stdout_path.exists():
            raise WindowsJobError(
                f"the job-object output already exists: {self.stdout_path}"
            )

        self._kernel32 = _kernel32()
        self._job = wintypes.HANDLE()
        self._process_info = PROCESS_INFORMATION()
        self._output = None
        self._resumed = False
        self._closed = False
        self._start_ns = time.time_ns()
        self._create()

    @property
    def pid(self) -> int:
        return int(self._process_info.dwProcessId)

    def _create(self):
        if self.stdout_path is not None:
            self.stdout_path.parent.mkdir(parents=True, exist_ok=True)

        self._job = self._kernel32.CreateJobObjectW(None, None)
        if not self._job:
            raise _win32_error("CreateJobObjectW")

        try:
            limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
            limits.BasicLimitInformation.LimitFlags = (
                self.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            )
            if not self._kernel32.SetInformationJobObject(
                self._job,
                self.JOB_OBJECT_EXTENDED_LIMIT_INFORMATION,
                ctypes.byref(limits),
                ctypes.sizeof(limits),
            ):
                raise _win32_error("SetInformationJobObject")

            startup = STARTUPINFOW()
            startup.cb = ctypes.sizeof(startup)
            inherit_handles = False
            output_handle = None
            if self.stdout_path is not None:
                self._output = self.stdout_path.open("xb")
                output_handle = msvcrt.get_osfhandle(self._output.fileno())
                os.set_handle_inheritable(output_handle, True)
                inherit_handles = True
                startup.dwFlags = self.STARTF_USESTDHANDLES
                startup.hStdInput = self._kernel32.GetStdHandle(
                    self.STD_INPUT_HANDLE
                )
                startup.hStdOutput = output_handle
                startup.hStdError = output_handle

            command_line = ctypes.create_unicode_buffer(
                subprocess.list2cmdline(self.command)
            )
            environment_block = "\0".join(
                f"{key}={value}"
                for key, value in sorted(
                    self.environment.items(), key=lambda item: item[0].upper()
                )
            ) + "\0\0"
            environment_buffer = ctypes.create_unicode_buffer(environment_block)
            try:
                created = self._kernel32.CreateProcessW(
                    self.command[0],
                    command_line,
                    None,
                    None,
                    inherit_handles,
                    self.CREATE_SUSPENDED | self.CREATE_UNICODE_ENVIRONMENT,
                    environment_buffer,
                    str(self.cwd),
                    ctypes.byref(startup),
                    ctypes.byref(self._process_info),
                )
            finally:
                if output_handle is not None:
                    os.set_handle_inheritable(output_handle, False)
            if not created:
                raise _win32_error("CreateProcessW")
            if not self._kernel32.AssignProcessToJobObject(
                self._job, self._process_info.hProcess
            ):
                raise _win32_error("AssignProcessToJobObject")
        except BaseException:
            self.close()
            raise

    def resume(self):
        if self._closed:
            raise WindowsJobError("cannot resume a closed job-object child")
        if self._resumed:
            raise WindowsJobError("the job-object child has already been resumed")
        if self._kernel32.ResumeThread(self._process_info.hThread) == 0xFFFFFFFF:
            raise _win32_error("ResumeThread")
        self._resumed = True

    def wait(
        self,
        timeout_seconds: float | None = None,
        *,
        timeout_exit_code: int = 124,
    ) -> dict:
        if self._closed:
            raise WindowsJobError("cannot wait for a closed job-object child")
        if not self._resumed:
            raise WindowsJobError("cannot wait before resuming the job-object child")
        if timeout_seconds is not None and timeout_seconds <= 0:
            raise WindowsJobError("the job-object timeout must be positive")

        wait_ms = (
            self.INFINITE
            if timeout_seconds is None
            else max(1, round(timeout_seconds * 1000))
        )
        wait_result = self._kernel32.WaitForSingleObject(
            self._process_info.hProcess, wait_ms
        )
        timed_out = wait_result == self.WAIT_TIMEOUT
        if timed_out:
            if not self._kernel32.TerminateJobObject(self._job, timeout_exit_code):
                raise _win32_error("TerminateJobObject")
            wait_result = self._kernel32.WaitForSingleObject(
                self._process_info.hProcess, self.TERMINATION_WAIT_MS
            )
        if wait_result != self.WAIT_OBJECT_0:
            raise _win32_error("WaitForSingleObject")

        exit_code = wintypes.DWORD()
        if not self._kernel32.GetExitCodeProcess(
            self._process_info.hProcess, ctypes.byref(exit_code)
        ):
            raise _win32_error("GetExitCodeProcess")
        if self._output is not None:
            self._output.flush()
        end_ns = time.time_ns()
        return {
            "pid": self.pid,
            "exit_code": int(exit_code.value),
            "timed_out": timed_out,
            "start_unix_ns": self._start_ns,
            "end_unix_ns": end_ns,
            "duration_ns": end_ns - self._start_ns,
            "stdout": str(self.stdout_path) if self.stdout_path else None,
        }

    def terminate(self, exit_code: int | None = None):
        if self._closed:
            return
        selected_exit_code = self.error_exit_code if exit_code is None else exit_code
        if self._job:
            self._kernel32.TerminateJobObject(self._job, selected_exit_code)
        if self._process_info.hProcess:
            wait_result = self._kernel32.WaitForSingleObject(
                self._process_info.hProcess, self.TERMINATION_WAIT_MS
            )
            if wait_result == self.WAIT_TIMEOUT:
                self._kernel32.TerminateProcess(
                    self._process_info.hProcess, selected_exit_code
                )
                self._kernel32.WaitForSingleObject(
                    self._process_info.hProcess, self.TERMINATION_WAIT_MS
                )

    def close(self):
        if self._closed:
            return
        if self._process_info.hProcess:
            exit_code = wintypes.DWORD()
            if self._kernel32.GetExitCodeProcess(
                self._process_info.hProcess, ctypes.byref(exit_code)
            ) and exit_code.value == self.STILL_ACTIVE:
                self.terminate()
        if self._output is not None:
            self._output.close()
            self._output = None
        if self._process_info.hThread:
            self._kernel32.CloseHandle(self._process_info.hThread)
            self._process_info.hThread = None
        if self._process_info.hProcess:
            self._kernel32.CloseHandle(self._process_info.hProcess)
            self._process_info.hProcess = None
        if self._job:
            self._kernel32.CloseHandle(self._job)
            self._job = None
        self._closed = True

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.close()
        return False


def run_in_job(
    command: list[str],
    environment: dict[str, str],
    cwd: Path,
    timeout_seconds: float,
    stdout_path: Path,
    *,
    timeout_exit_code: int = 124,
    error_exit_code: int = 125,
) -> dict:
    """Run a suspended child in a kill-on-close Windows job and capture output."""
    with SuspendedWindowsJob(
        command,
        environment,
        cwd,
        stdout_path,
        error_exit_code=error_exit_code,
    ) as process:
        process.resume()
        return process.wait(
            timeout_seconds, timeout_exit_code=timeout_exit_code
        )
