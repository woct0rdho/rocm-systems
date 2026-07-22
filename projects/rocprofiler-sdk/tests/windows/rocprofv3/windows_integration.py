from __future__ import annotations

import argparse
import csv
import json
import os
import shutil
import sqlite3
import subprocess
import sys
from pathlib import Path

WINDOWS_TEST_ROOT = Path(__file__).resolve().parents[1]
if str(WINDOWS_TEST_ROOT) not in sys.path:
    sys.path.insert(0, str(WINDOWS_TEST_ROOT))

from common.windows_job import run_in_job


CASES = (
    "availability",
    "baseline",
    "kernel-trace",
    "dispatch-analysis-contract",
    "hip-trace",
    "hip-graph",
    "hip-marker",
    "roctx-trace",
    "no-overwrite",
    "hsa-barrier",
)


def run_command(
    command: list[str],
    env: dict[str, str],
    stdout_path: Path,
    timeout: float = 60.0,
):
    result = run_in_job(command, env, Path.cwd(), timeout, stdout_path)
    stdout = stdout_path.read_text(encoding="utf-8", errors="replace")
    if result["timed_out"]:
        raise RuntimeError(
            f"command timed out after {timeout} seconds: "
            f"{subprocess.list2cmdline(command)}\n{stdout}"
        )
    return result["exit_code"], result["pid"], stdout


def base_environment(args) -> dict[str, str]:
    venv = args.venv.resolve()
    site_packages = venv / "Lib" / "site-packages"
    core = site_packages / "_rocm_sdk_core"
    devel = site_packages / "_rocm_sdk_devel"
    env = dict(os.environ)
    for name in (
        "GPU_CLR_PROFILE_OUTPUT",
        "HSA_TOOLS_LIB",
        "HSA_TOOLS_REPORT_LOAD_FAILURE",
        "ROCPROFILER_WINDOWS_HSA_TOOL_LOG",
        "ROCPROFILER_WINDOWS_TRACE_LOG",
        "ROCPROFILER_REGISTER_ENABLED",
        "ROCPROFILER_REGISTER_FORCE_LOAD",
        "ROCPROFILER_REGISTER_LIBRARY",
        "ROCPROFILER_REGISTER_SECURE",
    ):
        env.pop(name, None)
    env["PATH"] = os.pathsep.join(
        [
            str(args.runtime_root.resolve() / "bin"),
            str(args.sdk_build.resolve() / "bin"),
            str(core / "bin"),
            str(devel / "bin"),
            str(devel / "lib"),
            env.get("PATH", ""),
        ]
    )
    env["ROCPROF_LIST_AVAIL_TOOL_LIBRARY"] = str(
        (args.sdk_build.resolve() / "bin" / "rocprofv3-list-avail.dll")
    )
    env["ROCPROFILER_METRICS_PATH"] = str(
        args.repository_root.resolve()
        / "projects"
        / "rocprofiler-sdk"
        / "source"
        / "share"
        / "rocprofiler-sdk"
    )
    env["ROCPROFILER_WINDOWS_DLL_DIRS"] = os.pathsep.join(
        [
            str(args.sdk_build.resolve() / "bin"),
            str(args.runtime_root.resolve() / "bin"),
        ]
    )
    env["ROCPROFILER_WINDOWS_CORE_BIN"] = str(args.runtime_root.resolve() / "bin")
    return env


def prepare_runtime_run(runtime_root: Path, workload: Path, output: Path) -> Path:
    run_directory = output / "runtime-run"
    if run_directory.exists():
        shutil.rmtree(run_directory)
    run_directory.mkdir(parents=True)
    for name in ("amdhip64_7.dll", "amd_comgr.dll", "hsa-amd-aqlprofile64.dll"):
        source = runtime_root / "bin" / name
        destination = run_directory / name
        try:
            os.link(source, destination)
        except OSError:
            shutil.copy2(source, destination)
    prepared_workload = run_directory / workload.name
    shutil.copy2(workload, prepared_workload)
    shutil.copy2(workload.with_name("target-resource.txt"), run_directory)
    Path(f"{prepared_workload}.local").touch()
    return prepared_workload


def run_availability(args, env, output):
    representative_group = [
        "GRBM_COUNT",
        "GRBM_GUI_ACTIVE",
        "SQ_WAVES",
        "TA_TA_BUSY",
        "TCP_REQ",
        "GL1C_BUSY",
        "GL2C_HIT",
    ]
    rejected_group = [
        "GRBM_COUNT",
        "GRBM_CPC_BUSY",
        "GRBM_CPF_BUSY",
        "GRBM_CP_BUSY",
        "GRBM_EA_BUSY",
        "GRBM_GDS_BUSY",
        "GRBM_GUI_ACTIVE",
        "GRBM_SPI_BUSY",
        "GRBM_TA_BUSY",
        "GRBM_UTCL2_BUSY",
        "GRBM_GL2C_BUSY",
    ]
    commands = {
        "list": [str(args.python), str(args.rocprofv3), "--list-avail"],
        "agent": [str(args.python), str(args.rocprofv3_avail), "list", "--agent"],
        "catalog": [
            str(args.python),
            str(args.rocprofv3_avail),
            "info",
            "--pmc",
        ],
        "pmc_check": [
            str(args.python),
            str(args.rocprofv3_avail),
            "pmc-check",
            "GRBM_COUNT",
            "GRBM_GUI_ACTIVE",
            "SQ_WAVES",
        ],
        "pmc_check_derived": [
            str(args.python),
            str(args.rocprofv3_avail),
            "pmc-check",
            "GDSInsts",
        ],
        "pmc_check_representative": [
            str(args.python),
            str(args.rocprofv3_avail),
            "pmc-check",
            *representative_group,
        ],
        "pmc_check_catalog_boundaries": [
            str(args.python),
            str(args.rocprofv3_avail),
            "pmc-check",
            "GRBM_GL2C_BUSY",
            "GCEA_RDRAM_SIZE_REQ",
            "GCEA_WDRAM_SIZE_REQ",
        ],
        "pmc_check_rejected": [
            str(args.python),
            str(args.rocprofv3_avail),
            "pmc-check",
            *rejected_group,
        ],
        "pmc_check_unknown": [
            str(args.python),
            str(args.rocprofv3_avail),
            "pmc-check",
            "NO_SUCH_COUNTER",
        ],
    }
    results = {}
    for name, command in commands.items():
        returncode, _, stdout = run_command(command, env, output / f"{name}.stdout.txt")
        results[name] = {"command": command, "returncode": returncode, "stdout": stdout}
    return results


def run_workload(args, env, output):
    prepared = prepare_runtime_run(
        args.runtime_root.resolve(), args.workload.resolve(), output
    )
    run_env = dict(env)
    run_env["GPU_ENABLE_PAL"] = "0"
    command = [str(prepared)]
    returncode, _, stdout = run_command(
        command, run_env, output / "workload.stdout.txt"
    )
    shutil.rmtree(prepared.parent, ignore_errors=True)
    return {"command": command, "returncode": returncode, "stdout": stdout}


def run_hsa_barrier(args, env, output):
    tool_log = output / "hsa-tool.log"
    run_env = dict(env)
    sdk_path = args.sdk_build.resolve() / "bin" / "rocprofiler-sdk.dll"
    run_env["ROCPROFILER_REGISTER_ENABLED"] = "1"
    run_env["ROCPROFILER_REGISTER_FORCE_LOAD"] = "1"
    run_env["ROCPROFILER_REGISTER_LIBRARY"] = str(sdk_path)
    run_env["ROCPROFILER_REGISTER_SECURE"] = "1"
    run_env["ROCPROFILER_WINDOWS_TRACE_LOG"] = str(tool_log)
    for name in ("HSA_TOOLS_LIB", "HSA_TOOLS_REPORT_LOAD_FAILURE"):
        run_env.pop(name, None)

    venv = args.venv.resolve()
    site_packages = venv / "Lib" / "site-packages"
    command = [
        str(args.python),
        str(args.hsa_barrier_probe.resolve()),
        "--runtime",
        str(args.runtime_root.resolve() / "bin" / "amdhip64_7.dll"),
    ]
    for directory in (
        args.runtime_root.resolve() / "bin",
        args.sdk_build.resolve() / "bin",
        site_packages / "_rocm_sdk_core" / "bin",
        site_packages / "_rocm_sdk_devel" / "bin",
        site_packages / "_rocm_sdk_devel" / "lib",
    ):
        command.extend(("--dll-directory", str(directory)))

    returncode, _, stdout = run_command(
        command, run_env, output / "hsa-barrier.stdout.txt", timeout=30
    )
    return {
        "command": command,
        "returncode": returncode,
        "stdout": stdout,
        "tool_log": tool_log.read_text(encoding="utf-8") if tool_log.is_file() else "",
        "hsa_tools_lib": run_env.get("HSA_TOOLS_LIB"),
        "sdk_path": str(sdk_path),
        "registration_environment": {
            name: run_env.get(name)
            for name in (
                "ROCPROFILER_REGISTER_ENABLED",
                "ROCPROFILER_REGISTER_FORCE_LOAD",
                "ROCPROFILER_REGISTER_LIBRARY",
                "ROCPROFILER_REGISTER_SECURE",
            )
        },
    }


def run_hip_trace(args, env, output, graph: bool = False, marker: bool = False):
    trace_directory = output / (
        "hip-graph" if graph else ("hip-marker" if marker else "hip-trace")
    )
    trace_directory.mkdir()
    command = [
        str(args.python),
        str(args.rocprofv3),
        "--hip-graph-trace" if graph else "--hip-trace",
    ]
    if marker:
        command.append("--marker-trace")
    command.extend(
        [
            "--output-directory",
            str(trace_directory),
            "--output-file",
            "windows-hip",
            "--",
            str(args.workload.resolve()),
        ]
    )
    if graph:
        command.extend(("--graph", "--dispatches", "2"))
    if marker:
        command.extend(("--markers", "--dispatches", "2"))
    returncode, _, stdout = run_command(
        command, env, output / "hip-trace.stdout.txt", timeout=60
    )
    api_output = trace_directory / "windows-hip_hip_api_trace.csv"
    rows = []
    if api_output.is_file():
        with api_output.open(encoding="utf-8", newline="") as stream:
            rows = list(csv.DictReader(stream))
    graph_output = trace_directory / "windows-hip_hip_graph_trace.csv"
    graph_rows = []
    if graph_output.is_file():
        with graph_output.open(encoding="utf-8", newline="") as stream:
            graph_rows = list(csv.DictReader(stream))
    marker_api_output = trace_directory / "windows-hip_marker_api_trace.csv"
    marker_output = trace_directory / "windows-hip_marker_trace.csv"
    marker_api_rows = []
    marker_rows = []
    if marker_api_output.is_file():
        with marker_api_output.open(encoding="utf-8", newline="") as stream:
            marker_api_rows = list(csv.DictReader(stream))
    if marker_output.is_file():
        with marker_output.open(encoding="utf-8", newline="") as stream:
            marker_rows = list(csv.DictReader(stream))
    return {
        "command": command,
        "target": str(args.workload.resolve()),
        "returncode": returncode,
        "stdout": stdout,
        "rows": rows,
        "api_output": str(api_output),
        "graph_output_exists": graph_output.is_file(),
        "graph_rows": graph_rows,
        "marker_api_rows": marker_api_rows,
        "marker_rows": marker_rows,
    }


def run_roctx_trace(args, env, output):
    trace_directory = output / "roctx-trace"
    trace_directory.mkdir()
    command = [
        str(args.python),
        str(args.rocprofv3),
        "--marker-trace",
        "--output-directory",
        str(trace_directory),
        "--output-file",
        "windows-roctx",
        "--",
        str(args.roctx_workload.resolve()),
    ]
    returncode, _, stdout = run_command(
        command, env, output / "roctx-trace.stdout.txt", timeout=30
    )
    api_output = trace_directory / "windows-roctx_marker_api_trace.csv"
    marker_output = trace_directory / "windows-roctx_marker_trace.csv"
    api_rows = []
    marker_rows = []
    if api_output.is_file():
        with api_output.open(encoding="utf-8", newline="") as stream:
            api_rows = list(csv.DictReader(stream))
    if marker_output.is_file():
        with marker_output.open(encoding="utf-8", newline="") as stream:
            marker_rows = list(csv.DictReader(stream))
    return {
        "command": command,
        "returncode": returncode,
        "stdout": stdout,
        "api_output": str(api_output),
        "marker_output": str(marker_output),
        "api_rows": api_rows,
        "marker_rows": marker_rows,
    }


def run_kernel_trace(args, env, output):
    trace_directory = output / "trace"
    trace_directory.mkdir()
    command = [
        str(args.python),
        str(args.rocprofv3),
        "--kernel-trace",
        "--output-directory",
        str(trace_directory),
        "--output-file",
        "windows_test",
        "--",
        str(args.workload.resolve()),
    ]
    returncode, _, stdout = run_command(
        command, env, output / "kernel-trace.stdout.txt", timeout=90
    )
    csv_path = trace_directory / "windows_test_kernel_trace.csv"
    with csv_path.open(encoding="utf-8", newline="") as stream:
        rows = list(csv.DictReader(stream))
    return {
        "command": command,
        "target": str(args.workload.resolve()),
        "returncode": returncode,
        "stdout": stdout,
        "rows": rows,
    }


def read_csv_if_present(path: Path):
    if not path.is_file():
        return []
    with path.open(encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))


def run_dispatch_analysis_contract(args, env, output):
    contract = json.loads(args.dispatch_analysis_contract.read_text(encoding="utf-8"))
    workload = str(args.dispatch_analysis_workload.resolve())
    selection_names = {
        "no-filter",
        "include-vector",
        "exclude-lds",
        "include-and-exclude",
        "nonmatching",
        "vector-iteration-two",
        "vector-iteration-range",
        "mangled-vector",
        "truncated-vector",
        "composed-stats",
        "reversed-completion",
    }
    selection_cases = [
        case for case in contract["cases"] if case["name"] in selection_names
    ]
    selection_runs = []

    for case in selection_cases:
        case_directory = output / "selection" / case["name"]
        standalone_directory = case_directory / "standalone"
        composed_directory = case_directory / "composed"
        standalone_directory.mkdir(parents=True)
        composed_directory.mkdir()

        profiler_args = [
            value for value in case["profiler_args"] if value != "--kernel-trace"
        ]
        stats_requested = "--stats" in profiler_args
        standalone_command = [
            str(args.python),
            str(args.rocprofv3),
            "--kernel-trace",
            *profiler_args,
            "-f",
            "csv",
            "-d",
            str(standalone_directory),
            "-o",
            "contract",
            "--",
            workload,
            *case["target_args"],
        ]
        standalone_status, _, standalone_stdout = run_command(
            standalone_command,
            env,
            case_directory / "standalone.stdout.txt",
            timeout=120,
        )
        standalone_trace = standalone_directory / "contract_kernel_trace.csv"
        standalone_stats = standalone_directory / "contract_kernel_stats.csv"

        composed_command = [
            str(args.python),
            str(args.rocprofv3),
            "--kernel-trace",
            "--pmc",
            *contract["counter_group"],
            *profiler_args,
            "-f",
            "csv",
            "json",
            "-d",
            str(composed_directory),
            "-o",
            "contract",
            "--",
            workload,
            *case["target_args"],
        ]
        composed_status, _, composed_stdout = run_command(
            composed_command,
            env,
            case_directory / "composed.stdout.txt",
            timeout=120,
        )
        composed_counter = composed_directory / "contract_counter_collection.csv"
        composed_trace = composed_directory / "contract_kernel_trace.csv"
        composed_stats = composed_directory / "contract_kernel_stats.csv"
        composed_json = composed_directory / "contract_results.json"
        composed_json_kernels = []
        composed_json_symbols = []
        composed_json_summary = []
        if composed_json.is_file():
            document = json.loads(composed_json.read_text(encoding="utf-8"))
            tool_document = document["rocprofiler-sdk-tool"]
            if isinstance(tool_document, list):
                tool_document = tool_document[0]
            composed_json_kernels = tool_document["buffer_records"]["kernel_dispatch"]
            composed_json_symbols = tool_document["kernel_symbols"]
            composed_json_summary = tool_document["summary"]

        selection_runs.append(
            {
                "name": case["name"],
                "name_mode": case.get("name_mode", "demangled"),
                "stats_requested": stats_requested,
                "selected_enqueue_ordinals": case["selected_enqueue_ordinals"],
                "standalone": {
                    "command": standalone_command,
                    "returncode": standalone_status,
                    "stdout": standalone_stdout,
                    "trace_rows": read_csv_if_present(standalone_trace),
                    "trace_exists": standalone_trace.is_file(),
                    "stats_rows": read_csv_if_present(standalone_stats),
                    "stats_exists": standalone_stats.is_file(),
                },
                "composed": {
                    "command": composed_command,
                    "returncode": composed_status,
                    "stdout": composed_stdout,
                    "counter_rows": read_csv_if_present(composed_counter),
                    "trace_rows": read_csv_if_present(composed_trace),
                    "trace_exists": composed_trace.is_file(),
                    "stats_rows": read_csv_if_present(composed_stats),
                    "stats_exists": composed_stats.is_file(),
                    "json_exists": composed_json.is_file(),
                    "json_kernel_records": composed_json_kernels,
                    "json_kernel_symbols": composed_json_symbols,
                    "json_summary": composed_json_summary,
                },
            }
        )

    database_directory = output / "rocpd"
    database_directory.mkdir()
    no_filter = next(case for case in selection_cases if case["name"] == "no-filter")
    database_command = [
        str(args.python),
        str(args.rocprofv3),
        "--kernel-trace",
        "--stats",
        "--pmc",
        *contract["counter_group"],
        "-f",
        "rocpd",
        "-d",
        str(database_directory),
        "-o",
        "contract",
        "--",
        workload,
        *no_filter["target_args"],
    ]
    database_status, _, database_stdout = run_command(
        database_command,
        env,
        output / "rocpd.stdout.txt",
        timeout=120,
    )
    database_path = database_directory / "contract_results.db"
    database = {
        "command": database_command,
        "returncode": database_status,
        "stdout": database_stdout,
        "exists": database_path.is_file(),
        "internal_json_exists": (
            database_directory / "contract_results.json"
        ).is_file(),
        "metadata": {},
        "schema_objects": [],
        "integrity": None,
        "foreign_key_errors": [],
        "kernels": [],
        "pmc_events": [],
        "kernel_symbols": [],
        "top_kernels": [],
    }
    if database_path.is_file():
        with sqlite3.connect(database_path) as connection:
            connection.row_factory = sqlite3.Row
            database["metadata"] = dict(
                connection.execute("SELECT tag, value FROM rocpd_metadata").fetchall()
            )
            database["schema_objects"] = [
                row[0]
                for row in connection.execute(
                    "SELECT name FROM sqlite_master WHERE type IN ('table', 'view') "
                    "ORDER BY name"
                )
            ]
            database["integrity"] = connection.execute(
                "PRAGMA integrity_check"
            ).fetchone()[0]
            database["foreign_key_errors"] = [
                list(row) for row in connection.execute("PRAGMA foreign_key_check")
            ]
            database["kernels"] = [
                dict(row)
                for row in connection.execute(
                    "SELECT dispatch_id, kernel_id, name, tid, agent_abs_index, queue_id, "
                    "stream_id, start, end, duration, grid_x, grid_y, grid_z, workgroup_x, "
                    "workgroup_y, workgroup_z, lds_size, scratch_size, vgpr_count, "
                    "accum_vgpr_count, sgpr_count FROM kernels ORDER BY dispatch_id"
                )
            ]
            database["pmc_events"] = [
                dict(row)
                for row in connection.execute(
                    "SELECT dispatch_id, name, start, end, duration, counter_name, "
                    "counter_value FROM pmc_events ORDER BY dispatch_id, counter_name"
                )
            ]
            database["kernel_symbols"] = [
                dict(row)
                for row in connection.execute(
                    "SELECT kernel_id, formatted_kernel_name, group_segment_size, "
                    "private_segment_size, arch_vgpr_count, accum_vgpr_count, sgpr_count "
                    "FROM kernel_symbols ORDER BY kernel_id"
                )
            ]
            database["top_kernels"] = [
                dict(row)
                for row in connection.execute(
                    "SELECT name, total_calls, total_duration, average, percentage "
                    "FROM top_kernels ORDER BY total_duration DESC, name"
                )
            ]

    runs_by_name = {run["name"]: run for run in selection_runs}
    standalone = dict(runs_by_name["reversed-completion"]["standalone"])
    standalone["stats_exists"] = False
    composed = dict(runs_by_name["no-filter"]["composed"])
    composed["stats_exists"] = False
    return {
        "contract_version": contract["version"],
        "enqueue_sequence": contract["workload"]["enqueue_sequence"],
        "resource_metadata": contract["workload"][
            "gfx1151_windows_resource_metadata"
        ],
        "selection_cases": selection_runs,
        "standalone": standalone,
        "composed": composed,
        "rocpd": database,
    }


def run_no_overwrite(args, env, output):
    retained_output = output / "result_counter_collection.csv"
    retained_output.write_text("retained", encoding="utf-8")
    marker = output / "target-launched.txt"
    run_env = dict(env)
    command = [
        str(args.python),
        str(args.rocprofv3),
        "--pmc",
        "GRBM_COUNT",
        "--output-directory",
        str(output),
        "--output-file",
        "result",
        "--",
        str(Path(os.environ["SystemRoot"]) / "System32" / "cmd.exe"),
        "/c",
        f"echo launched>{marker}",
    ]
    returncode, _, stdout = run_command(
        command, run_env, output / "no-overwrite.stdout.txt"
    )
    return {
        "command": command,
        "returncode": returncode,
        "stdout": stdout,
        "retained_output": retained_output.read_text(encoding="utf-8"),
        "target_launched": marker.exists(),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--case", choices=CASES, required=True)
    parser.add_argument("--python", type=Path, required=True)
    parser.add_argument("--rocprofv3", type=Path, required=True)
    parser.add_argument("--rocprofv3-avail", type=Path, required=True)
    parser.add_argument("--runtime-root", type=Path, required=True)
    parser.add_argument("--workload", type=Path, required=True)
    parser.add_argument("--dispatch-analysis-workload", type=Path, required=True)
    parser.add_argument("--dispatch-analysis-contract", type=Path, required=True)
    parser.add_argument("--roctx-workload", type=Path, required=True)
    parser.add_argument("--hsa-barrier-probe", type=Path, required=True)
    parser.add_argument("--sdk-build", type=Path, required=True)
    parser.add_argument("--venv", type=Path, required=True)
    parser.add_argument("--repository-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    for path in (
        args.python,
        args.rocprofv3,
        args.rocprofv3_avail,
        args.runtime_root / "bin" / "amdhip64_7.dll",
        args.runtime_root / "bin" / "amd_comgr.dll",
        args.runtime_root / "bin" / "hsa-amd-aqlprofile64.dll",
        args.workload,
        args.dispatch_analysis_workload,
        args.dispatch_analysis_contract,
        args.roctx_workload,
        args.hsa_barrier_probe,
        args.sdk_build / "bin" / "rocprofiler-sdk.dll",
        args.sdk_build / "bin" / "rocprofv3-list-avail.dll",
    ):
        if not path.exists():
            raise FileNotFoundError(f"missing Windows integration input: {path}")

    output = args.output.resolve()
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    env = base_environment(args)

    if args.case == "availability":
        data = run_availability(args, env, output)
    elif args.case == "baseline":
        data = run_workload(args, env, output)
    elif args.case == "kernel-trace":
        data = run_kernel_trace(args, env, output)
    elif args.case == "dispatch-analysis-contract":
        data = run_dispatch_analysis_contract(args, env, output)
    elif args.case == "hip-trace":
        data = run_hip_trace(args, env, output)
    elif args.case == "hip-graph":
        data = run_hip_trace(args, env, output, graph=True)
    elif args.case == "hip-marker":
        data = run_hip_trace(args, env, output, marker=True)
    elif args.case == "roctx-trace":
        data = run_roctx_trace(args, env, output)
    elif args.case == "no-overwrite":
        data = run_no_overwrite(args, env, output)
    elif args.case == "hsa-barrier":
        data = run_hsa_barrier(args, env, output)
    else:
        raise AssertionError(args.case)

    record = {"case": args.case, "data": data}
    (output / "result.json").write_text(
        json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(f"windows_integration_case={args.case} result={output / 'result.json'}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise
