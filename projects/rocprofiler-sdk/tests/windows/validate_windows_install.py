from __future__ import annotations

import argparse
import csv
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import sqlite3
import subprocess
import sys
import time


COMMON = Path(__file__).resolve().parent / "common"
if str(COMMON) not in sys.path:
    sys.path.insert(0, str(COMMON))

from process_cleanup import assert_target_stopped
from windows_job import run_in_job


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def artifact(path: Path, prefix: Path) -> dict[str, object]:
    if not path.is_file():
        raise RuntimeError(f"installed artifact was not found: {path}")
    return {
        "path": path.relative_to(prefix).as_posix(),
        "size": path.stat().st_size,
        "sha256": sha256(path),
    }


def run_wrapper(
    wrapper: Path,
    arguments: list[str],
    cwd: Path,
    output: Path,
    target: Path | None = None,
    expected_outputs: tuple[Path, ...] = (),
) -> tuple[dict[str, object], str]:
    command_line = subprocess.list2cmdline([str(wrapper), *arguments])
    command = [os.environ.get("ComSpec", "cmd.exe"), "/d", "/s", "/c", command_line]
    environment = dict(os.environ)
    environment["ROCPROFV3_PYTHON"] = sys.executable
    for name in (
        "HSA_TOOLS_LIB",
        "ROCR_USE_PM4",
        "WSLKMT_VENDOR_PACKET",
    ):
        environment.pop(name, None)
    result = run_in_job(command, environment, cwd, 30.0, output)
    if target is not None:
        assert_target_stopped(target)
    text = output.read_text(encoding="utf-8", errors="replace")
    if result["timed_out"] or result["exit_code"] != 0:
        raise RuntimeError(
            f"installed rocprofv3 failed: exit_code={result['exit_code']} "
            f"timed_out={result['timed_out']}\n{text}"
        )
    missing = [path for path in expected_outputs if not path.is_file()]
    if missing:
        raise RuntimeError(
            "installed rocprofv3 returned success without publishing: "
            + ", ".join(str(path) for path in missing)
            + f"\n{text}"
        )
    return result, text


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--case-name", default="installed-prefix-process")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--fresh", action="store_true")
    parser.add_argument("--roctx-workload", type=Path)
    parser.add_argument("--hip-workload", type=Path)
    args = parser.parse_args()

    prefix = args.prefix.resolve()
    install_bin = prefix / "bin"
    wrapper = install_bin / "rocprofv3.cmd"
    list_avail = install_bin / "rocprofv3-list-avail.dll"
    required = [
        install_bin / "amdhip64_7.dll",
        install_bin / "amd_comgr.dll",
        install_bin / "hsa-amd-aqlprofile64.dll",
        install_bin / "rocprofiler-register.dll",
        install_bin / "rocprofiler-sdk.dll",
        install_bin / "rocprofiler-sdk-roctx.dll",
        install_bin / "rocprofiler-sdk-tool.dll",
        list_avail,
        install_bin / "rocprofv3",
        install_bin / "rocprofv3-avail",
        install_bin / "_rocprofv3_windows.py",
        install_bin / "_rocprofv3_windows_job.py",
        install_bin / "_rocprofv3_rocpd.py",
        wrapper,
        install_bin / "rocprofv3-avail.cmd",
        prefix / "include" / "rocprofiler-register" / "rocprofiler-register.h",
        prefix / "include" / "rocprofiler-sdk" / "rocprofiler.h",
        prefix / "include" / "rocprofiler-sdk" / "defines.h",
        prefix / "include" / "rocprofiler-sdk-roctx" / "roctx.h",
        prefix / "lib" / "amdhip64.lib",
        prefix / "lib" / "rocprofiler-register.lib",
        prefix / "lib" / "rocprofiler-sdk.lib",
        prefix / "lib" / "rocprofiler-sdk-roctx.lib",
        prefix / "lib" / "rocprofiler-sdk-tool.lib",
        prefix / "lib" / "rocprofiler-sdk" / "rocprofv3-list-avail.lib",
        prefix / "lib" / "cmake" / "rocprofiler-register" / "rocprofiler-register-config.cmake",
        prefix / "lib" / "cmake" / "rocprofiler-sdk" / "rocprofiler-sdk-config.cmake",
        prefix / "lib" / "python3" / "site-packages" / "rocprofv3" / "__init__.py",
        prefix / "lib" / "python3" / "site-packages" / "rocprofv3" / "avail.py",
        prefix / "share" / "rocprofiler-sdk" / "config.yaml",
        prefix / "share" / "rocprofiler-sdk-rocpd" / "rocpd_tables.sql",
        prefix / "share" / "rocprofiler-sdk-rocpd" / "data_views.sql",
        prefix / "share" / "rocprofiler-sdk-rocpd" / "summary_views.sql",
    ]
    artifacts = [artifact(path, prefix) for path in required]

    legacy_list_avail = install_bin / "rocprofiler-sdk" / "rocprofv3-list-avail.dll"
    if legacy_list_avail.exists():
        raise RuntimeError(f"obsolete nested availability DLL remains installed: {legacy_list_avail}")

    direct_load = subprocess.run(
        [
            sys.executable,
            "-c",
            "import ctypes, sys; ctypes.CDLL(sys.argv[1])",
            str(list_avail),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=30,
    )
    if direct_load.returncode != 0:
        raise RuntimeError(
            f"installed availability DLL failed direct loading: {list_avail}\n"
            f"{direct_load.stdout}"
        )

    defines_text = (prefix / "include" / "rocprofiler-sdk" / "defines.h").read_text(
        encoding="utf-8"
    )
    if (
        "ROCPROFILER_SDK_WINDOWS_MINIMAL_API" not in defines_text
        or "Consumers can use this macro" not in defines_text
        or "#if defined(_WIN32)" not in defines_text
    ):
        raise RuntimeError("installed headers do not expose the native Windows API boundary")

    required_exports = {
        "rocprofiler_assign_callback_thread",
        "rocprofiler_at_internal_thread_create",
        "rocprofiler_configure_buffer_dispatch_counting_service",
        "rocprofiler_configure_callback_dispatch_counting_service",
        "rocprofiler_configure_callback_tracing_service",
        "rocprofiler_configure_external_correlation_id_request_service",
        "rocprofiler_context_is_active",
        "rocprofiler_context_is_valid",
        "rocprofiler_create_buffer",
        "rocprofiler_create_callback_thread",
        "rocprofiler_create_context",
        "rocprofiler_create_counter",
        "rocprofiler_create_counter_config",
        "rocprofiler_destroy_buffer",
        "rocprofiler_destroy_counter_config",
        "rocprofiler_flush_buffer",
        "rocprofiler_force_configure",
        "rocprofiler_get_status_name",
        "rocprofiler_get_status_string",
        "rocprofiler_get_thread_id",
        "rocprofiler_get_timestamp",
        "rocprofiler_get_version",
        "rocprofiler_get_version_triplet",
        "rocprofiler_is_finalized",
        "rocprofiler_is_initialized",
        "rocprofiler_iterate_agent_supported_counters",
        "rocprofiler_iterate_counter_dimensions",
        "rocprofiler_iterate_runtime_registration_info",
        "rocprofiler_load_counter_definition",
        "rocprofiler_pop_external_correlation_id",
        "rocprofiler_push_external_correlation_id",
        "rocprofiler_query_available_agents",
        "rocprofiler_query_counter_info",
        "rocprofiler_query_counter_instance_count",
        "rocprofiler_query_external_correlation_id_request_kind_name",
        "rocprofiler_query_record_counter_id",
        "rocprofiler_query_record_dimension_position",
        "rocprofiler_set_api_table",
        "rocprofiler_start_context",
        "rocprofiler_stop_context",
    }
    dll_directories = []
    if hasattr(os, "add_dll_directory"):
        dll_directories.append(os.add_dll_directory(str(install_bin)))
    sdk_library = ctypes.WinDLL(str(install_bin / "rocprofiler-sdk.dll"))
    missing_exports = sorted(
        name for name in required_exports if getattr(sdk_library, name, None) is None
    )
    if missing_exports:
        raise RuntimeError(f"installed Windows SDK exports are incomplete: {missing_exports}")
    api_boundary = {
        "feature_macro": "ROCPROFILER_SDK_WINDOWS_MINIMAL_API",
        "required_plain_c_exports": len(required_exports),
        "missing_plain_c_exports": missing_exports,
    }

    if re.fullmatch(r"[a-z0-9-]+", args.case_name) is None:
        raise RuntimeError("case-name must contain only lowercase letters, digits, and hyphens")
    validation_root = args.output.resolve()
    if validation_root == prefix or prefix in validation_root.parents:
        raise RuntimeError("validation output must remain outside the installed package")
    if validation_root.exists() and args.fresh:
        shutil.rmtree(validation_root)
    validation_root.mkdir(parents=True, exist_ok=False)
    version_result, version_output = run_wrapper(
        wrapper, ["--version"], prefix, validation_root / "version.txt"
    )
    if not re.search(r"^\s*version:\s+1\.3\.5\s*$", version_output, re.MULTILINE):
        raise RuntimeError("installed rocprofv3 reported an unexpected version")
    if not re.search(r"^\s*system_name:\s+Windows\s*$", version_output, re.MULTILINE):
        raise RuntimeError("installed rocprofv3 did not report native Windows")

    help_result, help_output = run_wrapper(
        wrapper, ["--help"], prefix, validation_root / "help.txt"
    )
    if "ROCProfilerV3 Run Script" not in help_output or "--marker-trace" not in help_output:
        raise RuntimeError("installed rocprofv3 help is incomplete")

    for installed_launcher in (wrapper, install_bin / "rocprofv3-avail.cmd"):
        launcher_text = installed_launcher.read_text(encoding="ascii")
        if "ROCPROFV3_PYTHON" not in launcher_text or str(sys.executable) in launcher_text:
            raise RuntimeError(
                f"installed launcher is not relocatable: {installed_launcher}"
            )
    relocated_prefix = validation_root / "relocated-prefix"
    relocated_bin = relocated_prefix / "bin"
    try:
        relocated_bin.mkdir(parents=True)
        for launcher_artifact in (
            "rocprofv3",
            "rocprofv3.cmd",
            "_rocprofv3_windows.py",
            "_rocprofv3_windows_job.py",
            "_rocprofv3_rocpd.py",
        ):
            shutil.copy2(install_bin / launcher_artifact, relocated_bin)
        relocated_result, relocated_output = run_wrapper(
            relocated_bin / "rocprofv3.cmd",
            ["--version"],
            relocated_prefix,
            validation_root / "relocated-version.txt",
        )
        if not re.search(r"^\s*version:\s+1\.3\.5\s*$", relocated_output, re.MULTILINE):
            raise RuntimeError("relocated rocprofv3 launcher reported an unexpected version")
    finally:
        shutil.rmtree(relocated_prefix, ignore_errors=True)
    relocated_prefix_cleaned = not relocated_prefix.exists()
    if not relocated_prefix_cleaned:
        raise RuntimeError("relocated-prefix test copy was not removed")

    trace = None
    workload = args.roctx_workload.resolve() if args.roctx_workload else None
    if workload and workload.is_file():
        trace_directory = validation_root / "roctx"
        trace_result, trace_output = run_wrapper(
            wrapper,
            [
                "--marker-trace",
                "--output-directory",
                str(trace_directory),
                "--output-file",
                "windows-installed",
                "--",
                str(workload),
            ],
            prefix,
            validation_root / "roctx.txt",
            target=workload,
        )
        for marker in (
            "roctx_workload=passed",
            "hsa_initialized=no",
            "gpu_work_executed=no",
            "lifecycle=initialize,finalize",
        ):
            if marker not in trace_output:
                raise RuntimeError(f"installed ROCTX trace omitted: {marker}")
        api_rows = read_csv(trace_directory / "windows-installed_marker_api_trace.csv")
        marker_rows = read_csv(trace_directory / "windows-installed_marker_trace.csv")
        if len(api_rows) != 8 or len(marker_rows) != 4:
            raise RuntimeError(
                f"installed ROCTX row count mismatch: api={len(api_rows)} "
                f"markers={len(marker_rows)}"
            )
        trace = {
            "process": trace_result,
            "api_rows": len(api_rows),
            "marker_rows": len(marker_rows),
            "messages": [row["Message"] for row in marker_rows],
        }

    hip_trace = None
    hip_target_cleaned = None
    hip_workload = args.hip_workload.resolve() if args.hip_workload else None
    if hip_workload and hip_workload.is_file():
        hip_target_directory = validation_root / "hip-target"
        try:
            hip_target_directory.mkdir()
            installed_hip_target = hip_target_directory / hip_workload.name
            shutil.copy2(hip_workload, installed_hip_target)
            shutil.copy2(
                hip_workload.with_name("target-resource.txt"), hip_target_directory
            )
            for installed_dll in install_bin.glob("*.dll"):
                shutil.copy2(installed_dll, hip_target_directory)
            availability_result, availability_output = run_wrapper(
                wrapper, ["--list-avail"], prefix, validation_root / "availability.txt"
            )
            for marker in ("gfx1151", "GRBM_COUNT", "GRBM_GUI_ACTIVE", "SQ_WAVES"):
                if marker not in availability_output:
                    raise RuntimeError(f"installed availability omitted: {marker}")
            hip_directory = validation_root / "hip"
            hip_result, hip_output = run_wrapper(
                wrapper,
                [
                    "--hip-runtime-trace",
                    "--output-directory",
                    str(hip_directory),
                    "--output-file",
                    "windows-installed-hip",
                    "--",
                    str(installed_hip_target),
                    "--dispatches",
                    "2",
                ],
                prefix,
                validation_root / "hip.txt",
                target=installed_hip_target,
            )
            expected_runtime = str(
                (hip_target_directory / "amdhip64_7.dll").resolve()
            ).lower()
            for marker in (
                "architecture=gfx1151",
                "dispatches=2",
                "validation=passed",
                "lifecycle=initialize,finalize",
            ):
                if marker not in hip_output:
                    raise RuntimeError(f"installed HIP trace omitted: {marker}")
            if f"runtime={expected_runtime}" not in hip_output.lower():
                raise RuntimeError("installed HIP workload selected a competing runtime")
            hip_rows = read_csv(hip_directory / "windows-installed-hip_hip_api_trace.csv")
            function_counts: dict[str, int] = {}
            for row in hip_rows:
                function_counts[row["Function"]] = (
                    function_counts.get(row["Function"], 0) + 1
                )
            expected_counts = {
                "hipMalloc": 3,
                "hipMemcpy": 3,
                "hipLaunchKernel": 2,
                "hipDeviceSynchronize": 1,
                "hipFree": 3,
            }
            if function_counts != expected_counts:
                raise RuntimeError(
                    f"installed HIP API counts do not match: {function_counts}"
                )
            pmc_processes = []
            pmc_totals = []
            for iteration in range(1, 11):
                pmc_directory = validation_root / "pmc" / f"repeat-{iteration:02d}"
                output_name = f"windows-installed-pmc-{iteration:02d}"
                pmc_result, pmc_output = run_wrapper(
                    wrapper,
                    [
                        "--kernel-trace",
                        "--pmc",
                        "SQ_WAVES",
                        "--output-format",
                        "csv",
                        "json",
                        "--output-directory",
                        str(pmc_directory),
                        "--output-file",
                        output_name,
                        "--",
                        str(installed_hip_target),
                        "--dispatches",
                        "2",
                    ],
                    prefix,
                    validation_root / f"pmc-{iteration:02d}.txt",
                    target=installed_hip_target,
                    expected_outputs=(
                        pmc_directory / f"{output_name}_agent_info.csv",
                        pmc_directory / f"{output_name}_counter_collection.csv",
                        pmc_directory / f"{output_name}_kernel_trace.csv",
                        pmc_directory / f"{output_name}_results.json",
                    ),
                )
                for marker in (
                    "architecture=gfx1151",
                    "dispatches=2",
                    "validation=passed",
                ):
                    if marker not in pmc_output:
                        raise RuntimeError(
                            f"installed PMC run {iteration} omitted: {marker}"
                        )
                pmc_rows = read_csv(
                    pmc_directory / f"{output_name}_counter_collection.csv"
                )
                if len(pmc_rows) != 2:
                    raise RuntimeError(
                        f"installed PMC run {iteration} row count mismatch: {len(pmc_rows)}"
                    )
                dispatch_ids = [int(row["Dispatch_Id"]) for row in pmc_rows]
                if dispatch_ids != [1, 2]:
                    raise RuntimeError(
                        f"installed PMC run {iteration} dispatch IDs do not match: "
                        f"{dispatch_ids}"
                    )
                pmc_values = []
                for row in pmc_rows:
                    if (
                        row["Counter_Name"] != "SQ_WAVES"
                        or "vector_add" not in row["Kernel_Name"]
                    ):
                        raise RuntimeError(
                            f"installed PMC run {iteration} has unexpected identity: {row}"
                        )
                    pmc_values.append(float(row["Counter_Value"]))
                if any(value <= 0 for value in pmc_values):
                    raise RuntimeError(
                        f"installed PMC run {iteration} values are not positive: {pmc_values}"
                    )
                kernel_rows = read_csv(
                    pmc_directory / f"{output_name}_kernel_trace.csv"
                )
                if len(kernel_rows) != 2:
                    raise RuntimeError(
                        f"installed kernel run {iteration} row count mismatch: "
                        f"{len(kernel_rows)}"
                    )
                for counter, kernel in zip(pmc_rows, kernel_rows):
                    for field in (
                        "Agent_Id",
                        "Queue_Id",
                        "Thread_Id",
                        "Dispatch_Id",
                        "Kernel_Id",
                        "Kernel_Name",
                        "Correlation_Id",
                        "Start_Timestamp",
                        "End_Timestamp",
                    ):
                        if counter[field] != kernel[field]:
                            raise RuntimeError(
                                f"installed run {iteration} kernel/counter mismatch "
                                f"for {field}: {counter[field]} != {kernel[field]}"
                            )
                document = json.loads(
                    (pmc_directory / f"{output_name}_results.json").read_text(
                        encoding="utf-8"
                    )
                )
                tool_document = document["rocprofiler-sdk-tool"]
                if isinstance(tool_document, list):
                    tool_document = tool_document[0]
                pmc_json_records = tool_document["callback_records"][
                    "counter_collection"
                ]
                if len(pmc_json_records) != 2:
                    raise RuntimeError(
                        f"installed PMC run {iteration} JSON record count mismatch: "
                        f"{len(pmc_json_records)}"
                    )
                json_dispatch_ids = [
                    int(record["dispatch_data"]["dispatch_info"]["dispatch_id"])
                    for record in pmc_json_records
                ]
                if json_dispatch_ids != dispatch_ids:
                    raise RuntimeError(
                        f"installed PMC run {iteration} JSON dispatch IDs do not match: "
                        f"{json_dispatch_ids}"
                    )
                kernel_json_records = tool_document["buffer_records"][
                    "kernel_dispatch"
                ]
                if len(kernel_json_records) != 2:
                    raise RuntimeError(
                        f"installed kernel run {iteration} JSON record count mismatch: "
                        f"{len(kernel_json_records)}"
                    )
                if [
                    int(record["dispatch_info"]["dispatch_id"])
                    for record in kernel_json_records
                ] != dispatch_ids:
                    raise RuntimeError(
                        f"installed kernel run {iteration} JSON dispatch IDs do not match"
                    )
                pmc_processes.append(pmc_result)
                pmc_totals.append(sum(pmc_values))

            rocpd_directory = validation_root / "rocpd"
            rocpd_database = rocpd_directory / "windows-installed-rocpd_results.db"
            rocpd_result, rocpd_output = run_wrapper(
                wrapper,
                [
                    "--kernel-trace",
                    "--stats",
                    "--pmc",
                    "SQ_WAVES",
                    "--output-format",
                    "rocpd",
                    "--output-directory",
                    str(rocpd_directory),
                    "--output-file",
                    "windows-installed-rocpd",
                    "--",
                    str(installed_hip_target),
                    "--dispatches",
                    "2",
                ],
                prefix,
                validation_root / "rocpd.txt",
                target=installed_hip_target,
                expected_outputs=(rocpd_database,),
            )
            if "Windows ROCpd: dispatches=2" not in rocpd_output:
                raise RuntimeError("installed ROCpd run omitted its dispatch summary")
            if (rocpd_directory / "windows-installed-rocpd_results.json").exists():
                raise RuntimeError("installed ROCpd-only run retained its internal JSON")
            schema_manifest = json.loads(
                (
                    prefix
                    / "share/rocprofiler-sdk-rocpd/latest-schema.json"
                ).read_text(encoding="utf-8")
            )
            with sqlite3.connect(rocpd_database) as connection:
                integrity = connection.execute("PRAGMA integrity_check").fetchone()[0]
                foreign_key_errors = connection.execute(
                    "PRAGMA foreign_key_check"
                ).fetchall()
                schema_version = connection.execute(
                    "SELECT value FROM rocpd_metadata WHERE tag = 'schema_version'"
                ).fetchone()[0]
                user_version = connection.execute("PRAGMA user_version").fetchone()[0]
                database_kernels = connection.execute(
                    "SELECT dispatch_id, start, end, duration, vgpr_count, sgpr_count "
                    "FROM kernels ORDER BY dispatch_id"
                ).fetchall()
                database_pmc = connection.execute(
                    "SELECT dispatch_id, counter_name, counter_value FROM pmc_events "
                    "ORDER BY dispatch_id"
                ).fetchall()
                database_top = connection.execute(
                    "SELECT name, total_calls, total_duration, average, percentage "
                    "FROM top_kernels"
                ).fetchall()
            if integrity != "ok" or foreign_key_errors:
                raise RuntimeError(
                    f"installed ROCpd database is invalid: {integrity}, {foreign_key_errors}"
                )
            if (
                schema_version != schema_manifest["version"]
                or user_version != schema_manifest["user_version"]
            ):
                raise RuntimeError(
                    "installed ROCpd schema version does not match its manifest: "
                    f"{schema_version}, {user_version}"
                )
            if [row[0] for row in database_kernels] != [1, 2]:
                raise RuntimeError(
                    f"installed ROCpd dispatch IDs do not match: {database_kernels}"
                )
            if any(
                row[1] <= 0
                or row[2] <= row[1]
                or row[3] != row[2] - row[1]
                or row[4] <= 0
                or row[5] <= 0
                for row in database_kernels
            ):
                raise RuntimeError(
                    f"installed ROCpd kernel records are invalid: {database_kernels}"
                )
            if {row[0] for row in database_pmc} != {1, 2} or {
                row[1] for row in database_pmc
            } != {"SQ_WAVES"}:
                raise RuntimeError(
                    f"installed ROCpd counter identity does not match: {database_pmc}"
                )
            pmc_sums = {
                dispatch_id: sum(row[2] for row in database_pmc if row[0] == dispatch_id)
                for dispatch_id in (1, 2)
            }
            if any(value <= 0 for value in pmc_sums.values()):
                raise RuntimeError(
                    f"installed ROCpd counter values are not positive: {pmc_sums}"
                )
            if (
                len(database_top) != 1
                or "vector_add" not in database_top[0][0]
                or database_top[0][1] != 2
                or any(value <= 0 for value in database_top[0][2:4])
                or not math.isclose(database_top[0][4], 100.0)
            ):
                raise RuntimeError(
                    f"installed ROCpd top-kernel summary does not match: {database_top}"
                )
            rocpd_trace = {
                "process": rocpd_result,
                "schema_version": schema_version,
                "user_version": user_version,
                "integrity": integrity,
                "foreign_key_errors": foreign_key_errors,
                "kernel_rows": len(database_kernels),
                "pmc_rows": len(database_pmc),
                "pmc_sums": pmc_sums,
                "top_kernels": database_top,
            }
            hip_trace = {
                "availability_process": availability_result,
                "process": hip_result,
                "api_rows": len(hip_rows),
                "function_counts": function_counts,
                "runtime": expected_runtime,
                "pmc_process": pmc_processes[0],
                "pmc_processes": pmc_processes,
                "pmc_runs": len(pmc_processes),
                "pmc_rows_per_run": 2,
                "pmc_totals": pmc_totals,
                "pmc_json_records_per_run": 2,
                "kernel_rows_per_run": 2,
                "kernel_json_records_per_run": 2,
                "rocpd": rocpd_trace,
            }
        finally:
            shutil.rmtree(hip_target_directory, ignore_errors=True)
        hip_target_cleaned = not hip_target_directory.exists()
        if not hip_target_cleaned:
            raise RuntimeError("installed HIP target copy was not removed")

    record = {
        "record_type": "windows_rocprofiler_installed_prefix",
        "created_unix_ns": time.time_ns(),
        "prefix": str(prefix),
        "artifacts": artifacts,
        "api_boundary": api_boundary,
        "version_process": version_result,
        "help_process": help_result,
        "relocated_version_process": relocated_result,
        "roctx_trace": trace,
        "hip_trace": hip_trace,
        "disposable_cleanup": {
            "relocated_prefix": relocated_prefix_cleaned,
            "hip_target": hip_target_cleaned,
        },
        "hsa_initialized": hip_trace is not None,
        "gpu_work_executed": hip_trace is not None,
    }
    result_path = validation_root / "result.json"
    with result_path.open("x", encoding="utf-8", newline="") as stream:
        json.dump(record, stream, indent=2, sort_keys=True)
        stream.write("\n")
    print(
        "windows_installed_prefix=passed "
        f"roctx_trace={'yes' if trace is not None else 'not-built'} "
        f"hip_trace={'yes' if hip_trace is not None else 'not-built'} "
        f"pmc_trace={'yes' if hip_trace is not None else 'not-built'} "
        f"hsa_initialized={'yes' if hip_trace is not None else 'no'} "
        f"gpu_work_executed={'yes' if hip_trace is not None else 'no'}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
