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

import csv
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import uuid

from _rocprofv3_windows_job import SuspendedWindowsJob, WindowsJobError


fatal_error = None


def configure(fatal_error_callback):
    global fatal_error
    fatal_error = fatal_error_callback


def run_windows_availability(app_args):
    if app_args:
        fatal_error("--list-avail does not accept a target application on Windows")

    script_directory = os.path.dirname(os.path.realpath(__file__))
    candidates = [
        os.environ.get("ROCPROFV3_AVAIL_SCRIPT"),
        os.path.join(script_directory, "rocprofv3-avail.py"),
        os.path.join(script_directory, "rocprofv3-avail"),
        os.path.join(sys.prefix, "Scripts", "rocprofv3-avail.py"),
        os.path.join(sys.prefix, "bin", "rocprofv3-avail"),
    ]
    avail_script = next(
        (path for path in candidates if path and os.path.isfile(path)), None
    )
    if avail_script is None:
        fatal_error(
            "Could not locate rocprofv3-avail.py. Reinstall the Windows tools or set ROCPROFV3_AVAIL_SCRIPT"
        )

    app_env = dict(os.environ)
    app_env["ROCPROFILER_PC_SAMPLING_BETA_ENABLED"] = "on"
    for prefix in ("ROCPROF", "ROCPROFILER", "ROCTX"):
        app_env[f"{prefix}_LOG_LEVEL"] = "error"

    for command in (
        ["info", "--pmc"],
        ["info", "--pc-sampling"],
        ["info", "--spm-config"],
    ):
        subprocess.check_call([sys.executable, avail_script, *command], env=app_env)
    return 0


def windows_launch_in_job(command, environment, cwd, prepare=None):
    try:
        with SuspendedWindowsJob(command, environment, Path(cwd)) as process:
            if prepare is not None:
                prepare(process.pid)
            process.resume()
            return process.wait()
    except WindowsJobError as error:
        fatal_error("Windows target launch failed: {}", error)


def windows_output_reservation_path(output_path):
    return output_path.with_name(f".{output_path.name}.rocprofv3-reserve")


def windows_reserve_output_paths(output_paths):
    reservations = []
    token = f"pid={os.getpid()} token={uuid.uuid4().hex}\n"
    try:
        for output_path in dict.fromkeys(Path(path).resolve() for path in output_paths):
            output_path.parent.mkdir(parents=True, exist_ok=True)
            if output_path.exists():
                fatal_error("Windows trace output already exists: {}", output_path)
            reservation = windows_output_reservation_path(output_path)
            try:
                with reservation.open("x", encoding="utf-8", newline="") as output:
                    output.write(token)
            except FileExistsError:
                fatal_error("Windows trace output is already reserved: {}", output_path)
            reservations.append(reservation)
        return reservations
    except BaseException:
        windows_release_output_reservations(reservations)
        raise


def windows_release_output_reservations(reservations):
    for reservation in reservations:
        reservation.unlink(missing_ok=True)


def windows_core_bin():
    configured = os.environ.get("ROCPROFILER_WINDOWS_CORE_BIN")
    candidates = [
        Path(configured) if configured else None,
        Path(sys.prefix) / "Lib" / "site-packages" / "_rocm_sdk_core" / "bin",
    ]
    for candidate in candidates:
        if candidate and (candidate / "amdhip64_7.dll").is_file():
            return candidate.resolve()
    fatal_error(
        "Could not locate the venv amdhip64_7.dll. Set ROCPROFILER_WINDOWS_CORE_BIN to its bin directory"
    )


def windows_target_path(target):
    path = Path(target)
    if not path.is_file():
        located = shutil.which(target)
        if located:
            path = Path(located)
    if not path.is_file():
        fatal_error("Windows target executable was not found: {}", target)
    if path.suffix.lower() != ".exe":
        fatal_error(
            "Windows profiling currently requires a native .exe target: {}",
            path,
        )
    return path.resolve()


def windows_pid_path(path, process_id):
    return path.with_name(f"{path.stem}_{process_id}{path.suffix}")


def windows_dimensions(value, field):
    try:
        dimensions = tuple(int(item) for item in value.split("x"))
    except (AttributeError, TypeError, ValueError):
        fatal_error("Invalid {} in Windows HIP activity trace: {}", field, value)
    if len(dimensions) != 3 or any(item <= 0 for item in dimensions):
        fatal_error("Invalid {} in Windows HIP activity trace: {}", field, value)
    return dimensions


def windows_agent_identity(device_id):
    # Older HIP activity traces used a zero-based device id, while current CLR
    # traces carry the one-based WDDM driver node id. The constrained Windows
    # topology has one GPU agent in either representation, so preserve the
    # stable SDK identity Agent 1.
    return max(1, int(device_id))


def windows_parse_kernel_filter_range(values):
    text = " ".join(values or []).replace("[", " ").replace("]", " ")
    text = text.replace(",", " ")
    selected = set()
    for token in text.split():
        if "-" not in token:
            if not token.isdecimal():
                raise ValueError(f"expected an integer kernel iteration: {token}")
            selected.add(int(token))
            continue
        if token.count("-") != 1:
            raise ValueError(f"bad kernel iteration range: {token}")
        first_text, last_text = token.split("-", 1)
        if not first_text.isdecimal() or not last_text.isdecimal():
            raise ValueError(f"bad kernel iteration range: {token}")
        first = int(first_text)
        last = int(last_text)
        if first > last:
            raise ValueError(f"descending kernel iteration range: {token}")
        selected.update(range(first, last + 1))
    return selected


def windows_truncate_kernel_name(name):
    end = len(name.rstrip())
    token_end = end
    if end:
        close_to_open = {")": "(", ">": "<", "]": "["}
        close_token = name[end - 1]
        open_token = close_to_open.get(close_token)
        if open_token:
            depth = 1
            index = end - 2
            while index >= 0 and depth:
                if name[index] == close_token:
                    depth += 1
                elif name[index] == open_token:
                    depth -= 1
                index -= 1
            token_end = index + 1
    token_begin = token_end
    while token_begin > 0 and name[token_begin - 1] not in " :":
        token_begin -= 1
    return name[token_begin:token_end]


def windows_kernel_filter(args):
    include_expression = getattr(args, "kernel_include_regex", None) or ".*"
    exclude_expression = getattr(args, "kernel_exclude_regex", None)
    try:
        include_regex = re.compile(include_expression)
        exclude_regex = re.compile(exclude_expression) if exclude_expression else None
        iteration_range = windows_parse_kernel_filter_range(
            getattr(args, "kernel_iteration_range", None)
        )
    except (re.error, ValueError) as error:
        fatal_error("Invalid Windows kernel filter: {}", error)
    return include_regex, exclude_regex, iteration_range


def write_windows_kernel_csv(profile_path, output_path, process_id, args=None):
    try:
        profile = json.loads(profile_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fatal_error("Could not read Windows HIP activity trace {}: {}", profile_path, error)

    events = []
    for event in profile.get("traceEvents", []):
        event_args = event.get("args", {})
        if (
            event.get("ph") == "X"
            and isinstance(event_args, dict)
            and "grid" in event_args
            and "block" in event_args
        ):
            try:
                enqueue_ordinal = int(event_args["enqueue_ordinal"])
                operation_index = int(event_args["enqueue_operation_index"])
            except (KeyError, TypeError, ValueError):
                fatal_error(
                    "Windows HIP kernel activity is missing an explicit enqueue ordinal"
                )
            if enqueue_ordinal <= 0 or operation_index <= 0:
                fatal_error(
                    "Invalid Windows HIP kernel enqueue ordinal: {}:{}",
                    enqueue_ordinal,
                    operation_index,
                )
            events.append((enqueue_ordinal, operation_index, event))

    if not events:
        fatal_error(
            "The Windows HIP runtime produced no kernel activity records in {}",
            profile_path,
        )

    fields = [
        "Kind",
        "Agent_Id",
        "Queue_Id",
        "Stream_Id",
        "Thread_Id",
        "Dispatch_Id",
        "Kernel_Id",
        "Kernel_Name",
        "Correlation_Id",
        "Start_Timestamp",
        "End_Timestamp",
        "LDS_Block_Size",
        "Scratch_Size",
        "VGPR_Count",
        "Accum_VGPR_Count",
        "SGPR_Count",
        "Workgroup_Size_X",
        "Workgroup_Size_Y",
        "Workgroup_Size_Z",
        "Grid_Size_X",
        "Grid_Size_Y",
        "Grid_Size_Z",
    ]
    events.sort(key=lambda item: (item[0], item[1]))
    enqueue_keys = [(enqueue, operation) for enqueue, operation, _ in events]
    if len(enqueue_keys) != len(set(enqueue_keys)):
        fatal_error("Windows HIP kernel activity contains duplicate enqueue ordinals")

    if args is None:
        args = type("WindowsKernelArgs", (), {})()
    include_regex, exclude_regex, iteration_range = windows_kernel_filter(args)
    demangle = not getattr(args, "mangled_kernels", False)
    truncate = getattr(args, "truncate_kernels", False)

    kernel_ids = {}
    kernel_iterations = {}
    rows = []
    for dispatch_id, (enqueue_ordinal, _, event) in enumerate(events, start=1):
        event_args = event["args"]
        workgroup = windows_dimensions(event_args["block"], "block dimensions")
        block_grid = windows_dimensions(event_args["grid"], "grid dimensions")
        grid = tuple(block_grid[index] * workgroup[index] for index in range(3))
        demangled_name = str(event.get("name", ""))
        mangled_name = str(event_args.get("mangled_kernel_name", ""))
        if not demangle and not mangled_name:
            fatal_error("Windows HIP kernel activity is missing the mangled kernel name")
        kernel_name = demangled_name if demangle else mangled_name
        if truncate:
            kernel_name = windows_truncate_kernel_name(demangled_name)
        kernel_identity = mangled_name or demangled_name
        kernel_id = kernel_ids.setdefault(kernel_identity, len(kernel_ids) + 1)

        if not include_regex.search(kernel_name) or (
            exclude_regex and exclude_regex.search(kernel_name)
        ):
            continue
        iteration = kernel_iterations.get(kernel_name, 0) + 1
        kernel_iterations[kernel_name] = iteration
        if iteration_range and iteration not in iteration_range:
            continue

        begin_ns = round(float(event.get("ts", 0)) * 1000)
        end_ns = round(
            (float(event.get("ts", 0)) + float(event.get("dur", 0))) * 1000
        )
        if end_ns <= begin_ns:
            fatal_error(
                "Invalid Windows HIP kernel timestamps for dispatch {}: {} >= {}",
                dispatch_id,
                begin_ns,
                end_ns,
            )
        device_id = int(event.get("pid", 0))
        queue_id = int(event_args.get("queue_id", 0))
        stream_id = int(event.get("tid", 0))
        rows.append(
            {
                "Kind": "KERNEL_DISPATCH",
                "Agent_Id": f"Agent {windows_agent_identity(device_id)}",
                "Queue_Id": queue_id,
                "Stream_Id": stream_id,
                "Thread_Id": int(event_args.get("thread_id", process_id)),
                "Dispatch_Id": dispatch_id,
                "Kernel_Id": kernel_id,
                "Kernel_Name": kernel_name,
                "Correlation_Id": enqueue_ordinal,
                "Start_Timestamp": begin_ns,
                "End_Timestamp": end_ns,
                "LDS_Block_Size": 0,
                "Scratch_Size": 0,
                "VGPR_Count": 0,
                "Accum_VGPR_Count": 0,
                "SGPR_Count": 0,
                "Workgroup_Size_X": workgroup[0],
                "Workgroup_Size_Y": workgroup[1],
                "Workgroup_Size_Z": workgroup[2],
                "Grid_Size_X": grid[0],
                "Grid_Size_Y": grid[1],
                "Grid_Size_Z": grid[2],
            }
        )
    windows_write_csv(output_path, rows, fields)
    return len(rows)


def windows_kernel_output_path(args, process_id):
    output_directory = Path(args.output_directory or os.getcwd()).resolve()
    output_name = args.output_file or str(process_id)
    output_name = Path(output_name).name
    if not output_name.lower().endswith(".csv"):
        output_name = f"{output_name}_kernel_trace.csv"
    return output_directory / output_name


def run_windows_kernel_trace(app_args, args):
    if not app_args:
        fatal_error("--kernel-trace requires a target application on Windows")
    if args.output_format and any(value != "csv" for value in args.output_format):
        fatal_error("Windows kernel tracing currently supports CSV output only")
    if getattr(args, "input", None):
        fatal_error("Windows kernel tracing does not yet support --input")

    unsupported = (
        "runtime_trace",
        "sys_trace",
        "hip_trace",
        "marker_trace",
        "hip_graph_trace",
        "memory_copy_trace",
        "memory_allocation_trace",
        "kfd_trace",
        "scratch_memory_trace",
        "hsa_trace",
        "hip_runtime_trace",
        "hip_compiler_trace",
        "hsa_core_trace",
        "hsa_amd_trace",
        "hsa_image_trace",
        "hsa_finalizer_trace",
        "rccl_trace",
        "rocdecode_trace",
        "rocjpeg_trace",
        "pmc",
        "spm",
        "advanced_thread_trace",
    )
    selected = [name for name in unsupported if getattr(args, name, None)]
    if selected:
        fatal_error(
            "Windows kernel trace currently supports --kernel-trace only; unsupported options: {}",
            ", ".join(f"--{name.replace('_', '-')}" for name in selected),
        )

    core_bin = windows_core_bin()
    target = windows_target_path(app_args[0])

    profile_request = (
        Path(args.output_directory or os.getcwd()).resolve()
        / f".rocprofv3-windows-hip-{uuid.uuid4().hex}.json"
    )
    profile_request.parent.mkdir(parents=True, exist_ok=True)
    child_env = dict(os.environ)
    child_env["GPU_CLR_PROFILE_OUTPUT"] = str(profile_request)
    child_env["PATH"] = f"{core_bin}{os.pathsep}{child_env.get('PATH', '')}"
    for name in (
        "HSA_TOOLS_LIB",
        "HSA_TOOLS_REPORT_LOAD_FAILURE",
        "ROCPROFILER_WINDOWS_HSA_TOOL_LOG",
        "ROCR_USE_PM4",
        "WSLKMT_VENDOR_PACKET",
    ):
        child_env.pop(name, None)

    profile_path = None
    output_path = None
    reservations = []
    try:
        def prepare(process_id):
            nonlocal output_path, reservations
            output_path = windows_kernel_output_path(args, process_id)
            reservations = windows_reserve_output_paths([output_path])

        result = windows_launch_in_job(
            [str(target), *app_args[1:]], child_env, os.getcwd(), prepare
        )
        process_id = result["pid"]
        return_code = result["exit_code"]
        profile_path = windows_pid_path(profile_request, process_id)
        for _ in range(20):
            if profile_path.is_file():
                break
            time.sleep(0.1)
        if not profile_path.is_file():
            if return_code != 0:
                return return_code
            fatal_error(
                "The target did not produce a Windows HIP activity trace. The installed amdhip64_7.dll must export the HIP profiler extension"
            )
        record_count = write_windows_kernel_csv(
            profile_path, output_path, process_id, args
        )
        print(
            f"[rocprofv3] Windows kernel trace: records={record_count} output={output_path}",
            flush=True,
        )
        return return_code
    finally:
        if profile_path:
            profile_path.unlink(missing_ok=True)
        profile_request.unlink(missing_ok=True)
        windows_release_output_reservations(reservations)


def windows_api_trace_sdk_path():
    configured = os.environ.get("ROCPROFILER_WINDOWS_SDK_DLL")
    candidates = [Path(configured) if configured else None]
    for directory in os.environ.get("ROCPROFILER_WINDOWS_DLL_DIRS", "").split(
        os.pathsep
    ):
        if directory:
            candidates.append(Path(directory) / "rocprofiler-sdk.dll")
    candidates.extend(
        [
            Path(sys.prefix)
            / "Lib"
            / "site-packages"
            / "_rocm_sdk_core"
            / "bin"
            / "rocprofiler-sdk.dll",
            Path(__file__).resolve().parent / "rocprofiler-sdk.dll",
        ]
    )
    for candidate in candidates:
        if candidate and candidate.is_file():
            return candidate.resolve()
    fatal_error(
        "Could not locate rocprofiler-sdk.dll for Windows registration. Set ROCPROFILER_WINDOWS_SDK_DLL to its absolute path"
    )


def windows_api_trace_lifecycle_phases(trace_path):
    phases = []
    for line in trace_path.read_text(encoding="utf-8").splitlines():
        fields = dict(
            field.split("=", 1) for field in line.split() if "=" in field
        )
        if fields.get("event") == "sdk_lifecycle" and fields.get("phase"):
            phases.append(fields["phase"])
    return phases


def windows_api_trace_decode_text(value):
    try:
        return bytes.fromhex(value or "").decode("utf-8")
    except (UnicodeDecodeError, ValueError):
        fatal_error("Windows trace contains invalid UTF-8 text encoding")


def windows_api_trace_rows(trace_path):
    api_entries = {"hip_api": {}, "roctx_api": {}}
    api_rows = []
    graph_rows = []
    marker_entries = {}
    marker_rows = []
    for line in trace_path.read_text(encoding="utf-8").splitlines():
        fields = {}
        for field in line.split():
            if "=" in field:
                name, value = field.split("=", 1)
                fields[name] = value
        event = fields.get("event")
        if event in api_entries:
            correlation_id = fields.get("correlation_id")
            entries = api_entries[event]
            if fields.get("phase") == "enter" and correlation_id:
                if correlation_id in entries:
                    fatal_error(
                        "Windows trace has a duplicate enter correlation ID {}",
                        correlation_id,
                    )
                entries[correlation_id] = fields
            elif fields.get("phase") == "exit" and correlation_id:
                entry = entries.pop(correlation_id, None)
                if entry is None:
                    fatal_error(
                        "Windows trace has an unmatched exit correlation ID {}",
                        correlation_id,
                    )
                api_rows.append(
                    {
                        "Domain": (
                            "HIP_RUNTIME_API"
                            if event == "hip_api"
                            else "MARKER_CORE_API"
                        ),
                        "Function": fields.get("operation", entry.get("operation", "")),
                        "Process_Id": fields.get("process_id", entry.get("process_id", "0")),
                        "Thread_Id": fields.get("thread_id", entry.get("thread_id", "0")),
                        "Correlation_Id": correlation_id,
                        "Start_Timestamp": entry.get("timestamp_ns", "0"),
                        "End_Timestamp": fields.get("timestamp_ns", "0"),
                        "Status": fields.get("status", ""),
                    }
                )
        elif event == "hip_graph" and fields.get("phase") == "launch":
            graph_rows.append(
                {
                    "Kind": "HIP_GRAPH_LAUNCH",
                    "Graph_Exec_Id": fields.get("graph_exec_id", ""),
                    "Kernel_Dispatch_Count": fields.get("kernel_dispatch_count", "0"),
                    "Process_Id": fields.get("process_id", "0"),
                    "Thread_Id": fields.get("thread_id", "0"),
                    "Correlation_Id": fields.get("correlation_id", "0"),
                    "Timestamp": fields.get("timestamp_ns", "0"),
                    "Status": fields.get("status", ""),
                }
            )
        elif event == "roctx_marker":
            correlation_id = fields.get("correlation_id")
            phase = fields.get("phase")
            if not correlation_id:
                fatal_error("Windows ROCTX trace contains a marker without correlation")
            if phase == "enter":
                if correlation_id in marker_entries:
                    fatal_error(
                        "Windows ROCTX trace has a duplicate range correlation ID {}",
                        correlation_id,
                    )
                marker_entries[correlation_id] = fields
            elif phase in ("exit", "instant"):
                entry = (
                    marker_entries.pop(correlation_id, None)
                    if phase == "exit"
                    else fields
                )
                if entry is None:
                    fatal_error(
                        "Windows ROCTX trace has an unmatched range correlation ID {}",
                        correlation_id,
                    )
                marker_rows.append(
                    {
                        "Kind": fields.get("kind", entry.get("kind", "")),
                        "Operation": fields.get(
                            "operation", entry.get("operation", "")
                        ),
                        "Message": windows_api_trace_decode_text(
                            entry.get("message_hex", "")
                        ),
                        "Process_Id": entry.get("process_id", "0"),
                        "Thread_Id": entry.get("thread_id", "0"),
                        "Correlation_Id": correlation_id,
                        "Range_Id": fields.get("range_id", entry.get("range_id", "0")),
                        "Start_Timestamp": entry.get("timestamp_ns", "0"),
                        "End_Timestamp": fields.get("timestamp_ns", "0"),
                        "Status": fields.get("status", ""),
                    }
                )
    unmatched_api = [
        correlation_id
        for entries in api_entries.values()
        for correlation_id in entries
    ]
    if unmatched_api:
        fatal_error(
            "Windows trace has unmatched enter correlation IDs: {}",
            ", ".join(sorted(unmatched_api)),
        )
    if marker_entries:
        fatal_error(
            "Windows ROCTX trace has unmatched range correlation IDs: {}",
            ", ".join(sorted(marker_entries)),
        )
    return api_rows, graph_rows, marker_rows


def windows_api_trace_output_paths(args, process_id, output_directory=None):
    output_directory = Path(
        output_directory or args.output_directory or os.getcwd()
    ).resolve()
    output_name = Path(args.output_file or str(process_id)).name
    if output_name.lower().endswith(".csv"):
        output_name = output_name[:-4]
    return {
        "hip_api": output_directory / f"{output_name}_hip_api_trace.csv",
        "hip_graph": output_directory / f"{output_name}_hip_graph_trace.csv",
        "marker_api": output_directory / f"{output_name}_marker_api_trace.csv",
        "marker": output_directory / f"{output_name}_marker_trace.csv",
    }


def windows_write_csv(path, rows, fields):
    if path.exists():
        fatal_error("Windows trace output already exists: {}", path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{uuid.uuid4().hex}.tmp")
    try:
        with temporary.open("x", encoding="utf-8", newline="") as output:
            writer = csv.DictWriter(output, fieldnames=fields, quoting=csv.QUOTE_MINIMAL)
            writer.writeheader()
            writer.writerows(rows)
        try:
            temporary.rename(path)
        except FileExistsError:
            fatal_error("Windows trace output already exists: {}", path)
    finally:
        temporary.unlink(missing_ok=True)


def run_windows_api_trace(app_args, args, pass_id=None):
    if not app_args:
        fatal_error("Windows HIP/ROCTX tracing requires a target application")
    requested_formats = list(getattr(args, "output_format", None) or ["csv"])
    if any(value not in ("csv", "json") for value in requested_formats):
        fatal_error("Windows HIP/ROCTX tracing supports CSV trace output only")
    if "csv" not in requested_formats:
        fatal_error(
            "Windows HIP/ROCTX tracing requires CSV output; JSON may be requested "
            "in addition for composed counter records"
        )
    if getattr(args, "input", None):
        fatal_error("Windows HIP/ROCTX tracing does not yet support --input")

    hip_requested = any(
        getattr(args, name, False)
        for name in ("hip_trace", "hip_runtime_trace", "hip_graph_trace")
    )
    marker_requested = bool(getattr(args, "marker_trace", False))

    unsupported = (
        "runtime_trace",
        "sys_trace",
        "kernel_trace",
        "memory_copy_trace",
        "memory_allocation_trace",
        "kfd_trace",
        "scratch_memory_trace",
        "hsa_trace",
        "hip_compiler_trace",
        "hsa_core_trace",
        "hsa_amd_trace",
        "hsa_image_trace",
        "hsa_finalizer_trace",
        "rccl_trace",
        "rocdecode_trace",
        "rocjpeg_trace",
        "spm",
        "advanced_thread_trace",
    )
    selected = [name for name in unsupported if getattr(args, name, None)]
    if selected:
        fatal_error(
            "Windows HIP/ROCTX tracing cannot be combined with: {}",
            ", ".join(f"--{name.replace('_', '-')}" for name in selected),
        )

    core_bin = windows_core_bin()
    sdk_path = windows_api_trace_sdk_path()
    target = windows_target_path(app_args[0])
    output_directory = Path(args.output_directory or os.getcwd()).resolve()
    if pass_id is not None and getattr(args, "sub_directory", None):
        output_directory /= f"{args.sub_directory}{pass_id}"

    trace_path = output_directory / f".rocprofv3-windows-sdk-{uuid.uuid4().hex}.log"
    trace_path.parent.mkdir(parents=True, exist_ok=True)
    child_env = dict(os.environ)
    child_env["PATH"] = os.pathsep.join(
        [str(core_bin), str(sdk_path.parent), child_env.get("PATH", "")]
    )
    child_env["ROCPROFILER_REGISTER_ENABLED"] = "1"
    child_env["ROCPROFILER_REGISTER_FORCE_LOAD"] = "1"
    child_env["ROCPROFILER_REGISTER_LIBRARY"] = str(sdk_path)
    child_env["ROCPROFILER_REGISTER_SECURE"] = "1"
    child_env["ROCPROFILER_WINDOWS_TRACE_LOG"] = str(trace_path)
    counter_requested = getattr(args, "pmc", None) is not None
    counter_formats = requested_formats
    counter_output_file = None
    counter_result_path = None
    if counter_requested:
        counters = windows_sdk_counter_values(args, composed=True)
        tool_path = windows_sdk_library_path(
            "rocprofiler-sdk-tool.dll", "ROCPROFILER_WINDOWS_TOOL_DLL"
        )
        counter_output_file = windows_configure_sdk_counter_environment(
            child_env,
            args,
            counters,
            counter_formats,
            output_directory,
            getattr(args, "output_file", None),
            sdk_path,
            tool_path,
        )
        counter_result_path = windows_sdk_result_path(output_directory)
        child_env["ROCPROFILER_WINDOWS_RESULT_FILE"] = str(counter_result_path)
    for name in (
        "HSA_TOOLS_LIB",
        "HSA_TOOLS_REPORT_LOAD_FAILURE",
        "ROCPROFILER_WINDOWS_HSA_TOOL_LOG",
        "ROCR_USE_PM4",
        "WSLKMT_VENDOR_PACKET",
    ):
        child_env.pop(name, None)

    paths = None
    counter_paths = []
    reservations = []
    try:
        def prepare(process_id):
            nonlocal paths, counter_paths, reservations
            paths = windows_api_trace_output_paths(
                args, process_id, output_directory
            )
            output_paths = list(paths.values())
            if counter_requested:
                counter_paths = windows_sdk_output_paths(
                    output_directory,
                    counter_output_file,
                    counter_formats,
                    process_id,
                )
                output_paths.extend(counter_paths)
            reservations = windows_reserve_output_paths(output_paths)

        result = windows_launch_in_job(
            [str(target), *app_args[1:]], child_env, os.getcwd(), prepare
        )
        return_code = result["exit_code"]
        if counter_result_path is not None:
            return_code = windows_sdk_result_status(
                counter_result_path, return_code, counter_paths
            )
        if not trace_path.is_file():
            if return_code != 0:
                return return_code
            fatal_error(
                "The target produced no Windows SDK registration trace. Verify the matching rocprofiler-register.dll and rocprofiler-sdk.dll are discoverable"
            )
        lifecycle_phases = windows_api_trace_lifecycle_phases(trace_path)
        if lifecycle_phases.count("initialize") != 1 or lifecycle_phases.count("finalize") != 1:
            fatal_error(
                "The target did not complete the Windows SDK lifecycle: {}",
                ", ".join(lifecycle_phases) or "no lifecycle records",
            )
        api_rows, graph_rows, marker_rows = windows_api_trace_rows(trace_path)
        hip_api_rows = [row for row in api_rows if row["Domain"] == "HIP_RUNTIME_API"]
        marker_api_rows = [row for row in api_rows if row["Domain"] == "MARKER_CORE_API"]
        if hip_requested and not hip_api_rows:
            fatal_error("The target produced no Windows HIP API records")
        if getattr(args, "hip_graph_trace", False) and not graph_rows:
            fatal_error("The target produced no Windows HIP graph launch records")
        if marker_requested and (not marker_api_rows or not marker_rows):
            fatal_error("The target produced no Windows ROCTX marker records")

        requested_paths = []
        if hip_requested:
            requested_paths.append(paths["hip_api"])
        if getattr(args, "hip_graph_trace", False) or graph_rows:
            requested_paths.append(paths["hip_graph"])
        if marker_requested:
            requested_paths.extend((paths["marker_api"], paths["marker"]))
        existing_paths = [path for path in requested_paths if path.exists()]
        if existing_paths:
            fatal_error(
                "Windows trace output already exists: {}",
                ", ".join(str(path) for path in existing_paths),
            )

        api_fields = (
            "Domain",
            "Function",
            "Process_Id",
            "Thread_Id",
            "Correlation_Id",
            "Start_Timestamp",
            "End_Timestamp",
            "Status",
        )
        if hip_requested:
            windows_write_csv(paths["hip_api"], hip_api_rows, api_fields)
        if getattr(args, "hip_graph_trace", False) or graph_rows:
            windows_write_csv(
                paths["hip_graph"],
                graph_rows,
                (
                    "Kind",
                    "Graph_Exec_Id",
                    "Kernel_Dispatch_Count",
                    "Process_Id",
                    "Thread_Id",
                    "Correlation_Id",
                    "Timestamp",
                    "Status",
                ),
            )
        if marker_requested:
            windows_write_csv(paths["marker_api"], marker_api_rows, api_fields)
            windows_write_csv(
                paths["marker"],
                marker_rows,
                (
                    "Kind",
                    "Operation",
                    "Message",
                    "Process_Id",
                    "Thread_Id",
                    "Correlation_Id",
                    "Range_Id",
                    "Start_Timestamp",
                    "End_Timestamp",
                    "Status",
                ),
            )
        print(
            "[rocprofv3] Windows trace: "
            f"hip_records={len(hip_api_rows)} graph_records={len(graph_rows)} "
            f"marker_api_records={len(marker_api_rows)} marker_records={len(marker_rows)} "
            "lifecycle=initialize,finalize",
            flush=True,
        )
        return return_code
    finally:
        trace_path.unlink(missing_ok=True)
        if counter_result_path is not None:
            counter_result_path.unlink(missing_ok=True)
        windows_release_output_reservations(reservations)



def windows_sdk_library_path(name, environment_name):
    configured = os.environ.get(environment_name)
    candidates = [
        Path(configured) if configured else None,
        Path(__file__).resolve().parent / name,
    ]
    for candidate in candidates:
        if candidate and candidate.is_file():
            return candidate.resolve()
    fatal_error(
        "Could not locate {}. Reinstall the Windows tools or set {}",
        name,
        environment_name,
    )


def windows_sdk_result_path(output_directory):
    return Path(output_directory) / f".rocprofv3-windows-result-{uuid.uuid4().hex}.txt"


def windows_sdk_result_status(result_path, target_status, expected_outputs=()):
    if not result_path.is_file():
        try:
            with result_path.open("x", encoding="utf-8", newline="") as stream:
                stream.write(
                    "version=1\nstatus=success_no_dispatch\n"
                    "detail=target initialized no ROCm producer\n"
                )
        except FileExistsError:
            pass
        except OSError as error:
            fatal_error(
                "Could not publish the Windows no-dispatch result {}: {}",
                result_path,
                error,
            )
    try:
        values = {}
        for line in result_path.read_text(encoding="utf-8").splitlines():
            if "=" in line:
                name, value = line.split("=", 1)
                values[name] = value
    except (OSError, UnicodeError) as error:
        fatal_error("Could not read the Windows profiler result {}: {}", result_path, error)

    status = values.get("status", "invalid_result")
    if status == "success_records":
        missing = [str(path) for path in expected_outputs if not Path(path).is_file()]
        if missing:
            fatal_error(
                "Windows profiler reported records but did not publish: {}",
                ", ".join(missing),
            )
        return target_status
    if status in ("success_no_dispatch", "success_unknown_counter"):
        return target_status
    fatal_error(
        "Windows profiler failed ({}): {}",
        status,
        values.get("detail") or "no failure detail was provided",
    )


def windows_sdk_output_paths(
    output_directory, output_file, formats, process_id, kernel_trace=False
):
    prefix = str(output_file)
    replacements = {
        "%pid%": str(process_id),
        "{pid}": str(process_id),
        "%cwd%": os.getcwd(),
        "%hostname%": os.environ.get("COMPUTERNAME", "localhost"),
    }
    for key, value in replacements.items():
        prefix = prefix.replace(key, value)
    base = Path(output_directory) / prefix
    paths = []
    if "csv" in formats:
        paths.extend(
            [
                base.with_name(f"{base.name}_agent_info.csv"),
                base.with_name(f"{base.name}_counter_collection.csv"),
            ]
        )
        if kernel_trace:
            paths.append(base.with_name(f"{base.name}_kernel_trace.csv"))
    if "json" in formats:
        paths.append(base.with_name(f"{base.name}_results.json"))
    return paths


def windows_sdk_counter_values(args, composed=False):
    counters = getattr(args, "pmc", None) or []
    if counters and isinstance(counters[0], list):
        if len(counters) != 1:
            message = (
                "Windows composed profiling accepts one PMC group per pass"
                if composed
                else "Internal error: Windows PMC pass contains multiple counter groups"
            )
            fatal_error(message)
        counters = counters[0]
    counters = [str(value) for value in counters if str(value)]
    if not counters:
        fatal_error("--pmc requires at least one counter")
    return counters


def windows_configure_sdk_counter_environment(
    environment,
    args,
    counters,
    formats,
    output_directory,
    output_file,
    sdk_path,
    tool_path,
):
    path_directories = list(
        dict.fromkeys(
            (str(windows_core_bin()), str(sdk_path.parent), str(tool_path.parent))
        )
    )
    environment["PATH"] = os.pathsep.join(
        [*path_directories, environment.get("PATH", "")]
    ).rstrip(os.pathsep)
    environment["ROCPROFILER_REGISTER_ENABLED"] = "1"
    environment["ROCPROFILER_REGISTER_FORCE_LOAD"] = "1"
    environment["ROCPROFILER_REGISTER_SECURE"] = "1"
    environment["ROCPROFILER_REGISTER_LIBRARY"] = str(sdk_path)
    environment["ROCP_TOOL_LIBRARIES"] = str(tool_path)
    environment["ROCPROF_COUNTER_COLLECTION"] = "1"
    environment["ROCPROF_COUNTERS"] = f"pmc: {' '.join(counters)}"
    environment.pop("ROCPROF_COUNTER_GROUPS", None)
    environment["ROCPROF_OUTPUT_FORMAT"] = ",".join(formats)
    environment["ROCPROF_OUTPUT_PATH"] = str(output_directory)
    if output_file is None:
        output_file = environment.get("ROCPROF_OUTPUT_FILE_NAME", "%pid%")
    environment["ROCPROF_OUTPUT_FILE_NAME"] = str(output_file)

    metrics_directory = sdk_path.parent.parent / "share" / "rocprofiler-sdk"
    if (metrics_directory / "config.yaml").is_file():
        environment["ROCPROFILER_METRICS_PATH"] = str(metrics_directory)

    option_environment = {
        "ROCPROF_KERNEL_FILTER_INCLUDE_REGEX": getattr(
            args, "kernel_include_regex", None
        ),
        "ROCPROF_KERNEL_FILTER_EXCLUDE_REGEX": getattr(
            args, "kernel_exclude_regex", None
        ),
        "ROCPROF_KERNEL_FILTER_RANGE": ", ".join(
            getattr(args, "kernel_iteration_range", None) or []
        ),
        "ROCPROF_TRUNCATE_KERNELS": getattr(args, "truncate_kernels", False),
        "ROCPROF_DEMANGLE_KERNELS": not getattr(args, "mangled_kernels", False),
        "ROCPROF_SELECTED_REGIONS": getattr(args, "selected_regions", False),
        "ROCPROF_SELECTED_REGIONS_REF_COUNT": getattr(
            args, "selected_regions_ref_count", False
        ),
        "ROCPROF_KERNEL_TRACE": getattr(args, "kernel_trace", False),
        "ROCPROF_STATS": getattr(args, "stats", False),
    }
    for name, value in option_environment.items():
        if value not in (None, "", False):
            environment[name] = "1" if value is True else str(value)
        elif name == "ROCPROF_DEMANGLE_KERNELS":
            environment[name] = "0"
        else:
            environment.pop(name, None)
    return str(output_file)


def run_windows_sdk_pmc(app_args, args, pass_id=None):
    if not app_args:
        fatal_error("--pmc requires a target application on Windows")
    formats = list(getattr(args, "output_format", None) or ["csv"])
    unsupported = [value for value in formats if value not in ("csv", "json")]
    if unsupported:
        fatal_error(
            "Windows counter collection supports CSV and JSON output only (requested: {})",
            ", ".join(unsupported),
        )

    counters = windows_sdk_counter_values(args)

    target = windows_target_path(app_args[0])
    command = [str(target), *app_args[1:]]
    sdk_path = windows_sdk_library_path(
        "rocprofiler-sdk.dll", "ROCPROFILER_WINDOWS_SDK_DLL"
    )
    tool_path = windows_sdk_library_path(
        "rocprofiler-sdk-tool.dll", "ROCPROFILER_WINDOWS_TOOL_DLL"
    )

    environment = dict(os.environ)

    output_directory = Path(
        getattr(args, "output_directory", None) or os.getcwd()
    ).resolve()
    sub_directory = getattr(args, "sub_directory", None)
    if pass_id is not None and sub_directory:
        output_directory /= f"{sub_directory}{pass_id}"
    output_file = windows_configure_sdk_counter_environment(
        environment,
        args,
        counters,
        formats,
        output_directory,
        getattr(args, "output_file", None),
        sdk_path,
        tool_path,
    )

    result_path = windows_sdk_result_path(output_directory)
    environment["ROCPROFILER_WINDOWS_RESULT_FILE"] = str(result_path)
    expected_outputs = []
    reservations = []

    def prepare(process_id):
        nonlocal expected_outputs, reservations
        expected_outputs = windows_sdk_output_paths(
            output_directory,
            output_file,
            formats,
            process_id,
            kernel_trace=bool(getattr(args, "kernel_trace", False)),
        )
        reservations = windows_reserve_output_paths(expected_outputs)

    try:
        target_status = windows_launch_in_job(
            command, environment, os.getcwd(), prepare
        )["exit_code"]
        return windows_sdk_result_status(
            result_path, target_status, expected_outputs
        )
    finally:
        result_path.unlink(missing_ok=True)
        windows_release_output_reservations(reservations)


def run_windows(app_args, args, **kwargs):
    if getattr(args, "list_avail", False):
        return run_windows_availability(app_args)
    if getattr(args, "pmc", None) is not None:
        tracing_requested = any(
            getattr(args, name, False)
            for name in ("hip_trace", "hip_runtime_trace", "hip_graph_trace", "marker_trace")
        )
        if tracing_requested:
            return run_windows_api_trace(app_args, args, kwargs.get("pass_id"))
        return run_windows_sdk_pmc(app_args, args, kwargs.get("pass_id"))
    if any(
        getattr(args, name, False)
        for name in (
            "hip_trace",
            "hip_runtime_trace",
            "hip_graph_trace",
            "marker_trace",
        )
    ):
        return run_windows_api_trace(app_args, args)
    if not getattr(args, "kernel_trace", False):
        fatal_error(
            "Target application profiling requires --kernel-trace, --hip-trace, --hip-runtime-trace, --hip-graph-trace, or --marker-trace on Windows"
        )
    return run_windows_kernel_trace(app_args, args)


__all__ = tuple(
    name
    for name in globals()
    if name.startswith(("WINDOWS_", "windows_", "run_windows", "write_windows"))
)
