from __future__ import annotations

import csv
import ctypes
import datetime
import importlib.util
import json
import os
from pathlib import Path
import sqlite3
from types import SimpleNamespace

import pytest


RESOURCE_METADATA = {
    "resource_metadata_valid": True,
    "group_segment_size": 1024,
    "private_segment_size": 64,
    "arch_vgpr_count": 32,
    "accum_vgpr_count": 0,
    "sgpr_count": 128,
}


def load_frontend():
    script = Path(os.environ["ROCPROFV3_TEST_SCRIPT"]).resolve()
    spec = importlib.util.spec_from_file_location("rocprofv3_windows_unit", script)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not import rocprofv3 from {script}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_rocprofv3():
    module = load_frontend()
    return getattr(module, "_windows_backend", module)


def load_availability():
    script = Path(os.environ["ROCPROFV3_TEST_SCRIPT"]).resolve()
    availability_script = (
        script.parent.parent / "lib" / "python" / "rocprofv3" / "avail.py"
    )
    spec = importlib.util.spec_from_file_location(
        "rocprofv3_availability_unit", availability_script
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(
            f"could not import availability module from {availability_script}"
        )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture(scope="module")
def frontend():
    return load_frontend()


@pytest.fixture(scope="module")
def rocprofv3():
    return load_rocprofv3()


def require_fatal(capsys, callback, pattern: str):
    with pytest.raises(SystemExit) as error:
        callback()
    assert error.value.code == 1
    assert pattern in capsys.readouterr().err


def test_output_path_contract_resolves_all_static_keys(frontend):
    environment = {
        "CUSTOM_RANK": "3",
        "CUSTOM_SIZE": "8",
        "SLURM_JOB_ID": "42",
        "ROCPROF_TIME_FORMAT": "%F_%H.%M.%S",
        "ROCPROF_DATE_FORMAT": "%Y%m%d",
    }
    launched = datetime.datetime(2026, 7, 22, 13, 14, 15)
    command = [r"C:\Program Files\target.exe", " ./arg/one ", "two"]
    expected = {
        "argv": "C__Program Files_target.exe_arg_one_two",
        "argt": "target.exearg_one_two",
        "args": "arg_one_two",
        "tag": "target.exe",
        "arg0": "C__Program Files_target.exe",
        "arg1": "arg_one",
        "arg2": "two",
        "hostname": frontend.socket.gethostname(),
        "pid": "%pid%",
        "ppid": str(os.getpid()),
        "pgid": "%pid%",
        "psid": "%pid%",
        "psize": "1",
        "job": "42",
        "rank": "3",
        "size": "8",
        "nid": "3",
        "cwd": os.getcwd(),
        "launch_date": "20260722",
        "launch_time": "2026-07-22_13.14.15",
    }
    for key, value in expected.items():
        args = SimpleNamespace(
            output_file=f"%{key}%|{{{key}}}",
            output_directory=None,
            mpi_world_rank_variable="CUSTOM_RANK",
            mpi_world_size_variable="CUSTOM_SIZE",
        )
        frontend.normalize_output_path_keys(
            command, args, environment=environment, launch_time=launched
        )
        assert args.output_file == f"{value}|{value}"

    args = SimpleNamespace(
        output_file="%h|%p|%j|%r|%s",
        output_directory=None,
        mpi_world_rank_variable="CUSTOM_RANK",
        mpi_world_size_variable="CUSTOM_SIZE",
    )
    frontend.normalize_output_path_keys(
        command, args, environment=environment, launch_time=launched
    )
    assert args.output_file == (
        f"{frontend.socket.gethostname()}|%pid%|42|3|8"
    )


def test_output_path_contract_resolves_environment_and_missing_arguments(frontend):
    environment = {
        "NESTED": "%tag% nested/path",
        "CUSTOM_RANK": "0",
        "CUSTOM_SIZE": "1",
    }
    args = SimpleNamespace(
        output_file=(
            "%env{NESTED}%|$ENV{NESTED}|%q{NESTED}|"
            "before_%arg9%-after|before_{arg10}/after"
        ),
        output_directory="directory with spaces/%pid%",
        mpi_world_rank_variable="CUSTOM_RANK",
        mpi_world_size_variable="CUSTOM_SIZE",
    )
    frontend.normalize_output_path_keys(
        [r"C:\tools\target.exe"],
        args,
        environment=environment,
        launch_time=datetime.datetime(2026, 7, 22),
    )
    assert args.output_file == (
        "target.exe_nested_path|target.exe_nested_path|target.exe_nested_path|"
        "before_after|before_after"
    )
    assert args.output_directory == "directory with spaces/%pid%"


def test_output_path_nid_uses_process_without_mpi(frontend):
    args = SimpleNamespace(
        output_file="%nid%|{nid}",
        output_directory=None,
        mpi_world_rank_variable=None,
        mpi_world_size_variable=None,
    )
    frontend.normalize_output_path_keys(
        [r"C:\tools\target.exe"],
        args,
        environment={},
        launch_time=datetime.datetime(2026, 7, 22),
    )
    assert args.output_file == "%pid%|%pid%"

    args.output_file = "%nid%|{nid}"
    args.mpi_world_rank_variable = "CUSTOM_RANK"
    args.mpi_world_size_variable = "CUSTOM_SIZE"
    frontend.normalize_output_path_keys(
        [r"C:\tools\target.exe"],
        args,
        environment={"CUSTOM_RANK": "0", "CUSTOM_SIZE": "1"},
        launch_time=datetime.datetime(2026, 7, 22),
    )
    assert args.output_file == "0|0"


def test_kernel_trace_conversion_normalizes_agent_identity(rocprofv3, tmp_path, capsys):
    activity = tmp_path / "activity.json"
    activity.write_text(
        json.dumps(
            {
                "traceEvents": [
                    {
                        "ph": "X",
                        "name": "vector_add",
                        "pid": device_id,
                        "tid": 2,
                        "ts": float(device_id + 1),
                        "dur": 0.5,
                        "args": {
                            "grid": "4096x1x1",
                            "block": "256x1x1",
                            "queue_id": device_id,
                            "enqueue_ordinal": device_id + 1,
                            "enqueue_operation_index": 1,
                            "mangled_kernel_name": "_Z10vector_addv",
                            "thread_id": 100 + device_id,
                            **RESOURCE_METADATA,
                        },
                    }
                    for device_id in (0, 1)
                ]
            }
        ),
        encoding="utf-8",
    )
    output = tmp_path / "kernel_trace.csv"
    assert rocprofv3.write_windows_kernel_csv(activity, output, 4242) == 2
    with output.open(encoding="utf-8", newline="") as stream:
        rows = list(csv.DictReader(stream))
    assert [row["Agent_Id"] for row in rows] == ["Agent 1", "Agent 1"]
    assert [int(row["Queue_Id"]) for row in rows] == [0, 1]
    assert [int(row["LDS_Block_Size"]) for row in rows] == [1024, 1024]
    assert [int(row["Scratch_Size"]) for row in rows] == [64, 64]
    assert [int(row["VGPR_Count"]) for row in rows] == [32, 32]
    assert [int(row["Accum_VGPR_Count"]) for row in rows] == [0, 0]
    assert [int(row["SGPR_Count"]) for row in rows] == [128, 128]
    retained = output.read_bytes()
    require_fatal(
        capsys,
        lambda: rocprofv3.write_windows_kernel_csv(activity, output, 4242),
        "output already exists",
    )
    assert output.read_bytes() == retained


@pytest.mark.parametrize(
    ("missing_field", "resource_override", "message"),
    [
        (None, {"resource_metadata_valid": False}, "contains invalid resource fields"),
        ("sgpr_count", {}, "is missing resource fields"),
    ],
)
def test_kernel_trace_rejects_missing_or_invalid_resource_metadata(
    rocprofv3,
    tmp_path,
    capsys,
    missing_field,
    resource_override,
    message,
):
    activity = tmp_path / "missing-resource.json"
    event_args = {
        "grid": "1x1x1",
        "block": "64x1x1",
        "queue_id": 1,
        "enqueue_ordinal": 1,
        "enqueue_operation_index": 1,
        "mangled_kernel_name": "_Z10vector_addv",
        "thread_id": 100,
        **RESOURCE_METADATA,
        **resource_override,
    }
    if missing_field is not None:
        event_args.pop(missing_field)
    activity.write_text(
        json.dumps(
            {
                "traceEvents": [
                    {
                        "ph": "X",
                        "name": "vector_add",
                        "pid": 0,
                        "tid": 2,
                        "ts": 1.0,
                        "dur": 0.5,
                        "args": event_args,
                    }
                ]
            }
        ),
        encoding="utf-8",
    )

    require_fatal(
        capsys,
        lambda: rocprofv3.write_windows_kernel_csv(
            activity, tmp_path / "missing-resource.csv", 4242
        ),
        f"kernel_metadata_missing: Windows HIP kernel activity {message}",
    )


def test_kernel_trace_filters_explicit_enqueue_order_and_formatted_name(
    rocprofv3, tmp_path
):
    sequence = [
        (1, "dispatch_vector(float const*, float*)", "_Z15dispatch_vectorPKfPf"),
        (2, "dispatch_lds_conflict(float*)", "_Z21dispatch_lds_conflictPf"),
        (3, "dispatch_vector(float const*, float*)", "_Z15dispatch_vectorPKfPf"),
        (4, "dispatch_lds_conflict(float*)", "_Z21dispatch_lds_conflictPf"),
        (5, "dispatch_vector(float const*, float*)", "_Z15dispatch_vectorPKfPf"),
        (6, "dispatch_resource(float*)", "_Z17dispatch_resourcePf"),
    ]
    by_ordinal = {
        ordinal: {
            "ph": "X",
            "name": name,
            "pid": 1,
            "tid": ordinal % 2 + 2,
            "ts": float(ordinal),
            "dur": 0.25,
            "args": {
                "grid": "4x1x1",
                "block": "64x1x1",
                "queue_id": ordinal % 2 + 2,
                "enqueue_ordinal": ordinal,
                "enqueue_operation_index": 1,
                "mangled_kernel_name": mangled,
                "thread_id": 700,
                **RESOURCE_METADATA,
            },
        }
        for ordinal, name, mangled in sequence
    }
    activity = tmp_path / "reversed-activity.json"
    activity.write_text(
        json.dumps(
            {"traceEvents": [by_ordinal[ordinal] for ordinal in (5, 2, 6, 3, 1, 4)]}
        ),
        encoding="utf-8",
    )

    ranged = tmp_path / "ranged.csv"
    ranged_args = SimpleNamespace(
        kernel_include_regex="dispatch_vector",
        kernel_exclude_regex=None,
        kernel_iteration_range=["[2]"],
        mangled_kernels=False,
        truncate_kernels=False,
    )
    assert rocprofv3.write_windows_kernel_csv(activity, ranged, 4242, ranged_args) == 1
    with ranged.open(encoding="utf-8", newline="") as stream:
        ranged_rows = list(csv.DictReader(stream))
    assert [int(row["Dispatch_Id"]) for row in ranged_rows] == [3]
    assert [int(row["Correlation_Id"]) for row in ranged_rows] == [3]
    assert [row["Kernel_Name"] for row in ranged_rows] == [
        "dispatch_vector(float const*, float*)"
    ]

    mangled = tmp_path / "mangled.csv"
    mangled_args = SimpleNamespace(
        kernel_include_regex="dispatch_vector",
        kernel_exclude_regex=None,
        kernel_iteration_range=[],
        mangled_kernels=True,
        truncate_kernels=False,
    )
    assert (
        rocprofv3.write_windows_kernel_csv(activity, mangled, 4242, mangled_args) == 3
    )
    with mangled.open(encoding="utf-8", newline="") as stream:
        mangled_rows = list(csv.DictReader(stream))
    assert all(
        row["Kernel_Name"].startswith("_Z15dispatch_vector") for row in mangled_rows
    )

    truncated = tmp_path / "truncated.csv"
    truncated_args = SimpleNamespace(
        kernel_include_regex="^dispatch_vector$",
        kernel_exclude_regex=None,
        kernel_iteration_range=[],
        mangled_kernels=False,
        truncate_kernels=True,
    )
    assert (
        rocprofv3.write_windows_kernel_csv(activity, truncated, 4242, truncated_args)
        == 3
    )
    with truncated.open(encoding="utf-8", newline="") as stream:
        truncated_rows = list(csv.DictReader(stream))
    assert [row["Dispatch_Id"] for row in truncated_rows] == ["1", "3", "5"]
    assert {row["Kernel_Name"] for row in truncated_rows} == {"dispatch_vector"}


def test_kernel_statistics_use_linux_sample_accumulator(rocprofv3):
    rows = [
        {
            "Kernel_Name": "dispatch_vector",
            "Start_Timestamp": "100",
            "End_Timestamp": "110",
        },
        {
            "Kernel_Name": "dispatch_vector",
            "Start_Timestamp": "200",
            "End_Timestamp": "220",
        },
        {
            "Kernel_Name": "dispatch_resource",
            "Start_Timestamp": "300",
            "End_Timestamp": "330",
        },
    ]
    stats = rocprofv3.windows_kernel_stats_rows(rows)
    assert [row["Name"] for row in stats] == [
        "dispatch_resource",
        "dispatch_vector",
    ]
    by_name = {row["Name"]: row for row in stats}
    assert by_name["dispatch_vector"] == {
        "Name": "dispatch_vector",
        "Calls": 2,
        "TotalDurationNs": 30,
        "AverageNs": "15.000000",
        "Percentage": "50.00",
        "MinNs": 10,
        "MaxNs": 20,
        "StdDev": "7.071068",
    }
    assert by_name["dispatch_resource"]["Calls"] == 1
    assert by_name["dispatch_resource"]["StdDev"] == "0.00000000e+00"
    assert sum(float(row["Percentage"]) for row in stats) == pytest.approx(100.0)


def test_hip_and_graph_trace_conversion(rocprofv3, tmp_path):
    trace = tmp_path / "trace.log"
    trace.write_text(
        "event=hip_api phase=enter operation=hipMalloc correlation_id=41 "
        "process_id=100 thread_id=200 timestamp_ns=1000 bytes=4096\n"
        "event=hip_api phase=exit operation=hipMalloc correlation_id=41 "
        "process_id=100 thread_id=200 timestamp_ns=1100 status=0\n"
        "event=hip_api phase=enter operation=hipGraphLaunch correlation_id=42 "
        "process_id=100 thread_id=200 timestamp_ns=1200 graph_exec=0x1234\n"
        "event=hip_graph phase=launch graph_exec_id=0x1234 "
        "kernel_dispatch_count=2 correlation_id=42 process_id=100 thread_id=200 "
        "timestamp_ns=1250 status=0\n"
        "event=hip_api phase=exit operation=hipGraphLaunch correlation_id=42 "
        "process_id=100 thread_id=200 timestamp_ns=1300 status=0\n",
        encoding="utf-8",
    )
    api_rows, graph_rows, marker_rows = rocprofv3.windows_api_trace_rows(trace)
    assert [row["Function"] for row in api_rows] == ["hipMalloc", "hipGraphLaunch"]
    assert [row["Correlation_Id"] for row in api_rows] == ["41", "42"]
    assert all(row["Domain"] == "HIP_RUNTIME_API" for row in api_rows)
    assert int(api_rows[0]["End_Timestamp"]) > int(api_rows[0]["Start_Timestamp"])
    assert marker_rows == []
    assert graph_rows == [
        {
            "Kind": "HIP_GRAPH_LAUNCH",
            "Graph_Exec_Id": "0x1234",
            "Kernel_Dispatch_Count": "2",
            "Process_Id": "100",
            "Thread_Id": "200",
            "Correlation_Id": "42",
            "Timestamp": "1250",
            "Status": "0",
        }
    ]


def test_windows_lifecycle_conversion(rocprofv3, tmp_path):
    trace = tmp_path / "lifecycle.log"
    trace.write_text(
        "event=sdk_lifecycle phase=initialize process_id=1 thread_id=2 timestamp_ns=3\n"
        "event=sdk_lifecycle phase=finalize process_id=1 thread_id=2 timestamp_ns=4\n",
        encoding="utf-8",
    )
    assert rocprofv3.windows_api_trace_lifecycle_phases(trace) == [
        "initialize",
        "finalize",
    ]


def test_roctx_trace_conversion(rocprofv3, tmp_path):
    trace = tmp_path / "roctx.log"
    trace.write_text(
        "event=roctx_api phase=enter operation=roctxMarkA correlation_id=43 "
        "process_id=100 thread_id=200 timestamp_ns=1400 message_hex=6d61726b206f6e65\n"
        "event=roctx_marker operation=roctxMarkA kind=mark phase=instant "
        "correlation_id=43 range_id=0 process_id=100 thread_id=200 "
        "timestamp_ns=1400 status=0 message_hex=6d61726b206f6e65\n"
        "event=roctx_api phase=exit operation=roctxMarkA correlation_id=43 "
        "process_id=100 thread_id=200 timestamp_ns=1450 status=0\n"
        "event=roctx_api phase=enter operation=roctxRangeStartA correlation_id=44 "
        "process_id=100 thread_id=200 timestamp_ns=1500 message_hex=70726f636573732072616e6765\n"
        "event=roctx_marker operation=roctxRangeStartA kind=process_range phase=enter "
        "correlation_id=44 range_id=7 process_id=100 thread_id=200 "
        "timestamp_ns=1500 status=0 message_hex=70726f636573732072616e6765\n"
        "event=roctx_api phase=exit operation=roctxRangeStartA correlation_id=44 "
        "process_id=100 thread_id=200 timestamp_ns=1550 status=0\n"
        "event=roctx_api phase=enter operation=roctxRangeStop correlation_id=45 "
        "process_id=100 thread_id=201 timestamp_ns=1600 range_id=7\n"
        "event=roctx_marker operation=roctxRangeStartA kind=process_range phase=exit "
        "correlation_id=44 range_id=7 process_id=100 thread_id=200 "
        "timestamp_ns=1650 status=0 message_hex=70726f636573732072616e6765\n"
        "event=roctx_api phase=exit operation=roctxRangeStop correlation_id=45 "
        "process_id=100 thread_id=201 timestamp_ns=1700 status=0\n",
        encoding="utf-8",
    )
    api_rows, graph_rows, marker_rows = rocprofv3.windows_api_trace_rows(trace)
    assert graph_rows == []
    assert [row["Function"] for row in api_rows] == [
        "roctxMarkA",
        "roctxRangeStartA",
        "roctxRangeStop",
    ]
    assert {row["Domain"] for row in api_rows} == {"MARKER_CORE_API"}
    assert marker_rows == [
        {
            "Kind": "mark",
            "Operation": "roctxMarkA",
            "Message": "mark one",
            "Process_Id": "100",
            "Thread_Id": "200",
            "Correlation_Id": "43",
            "Range_Id": "0",
            "Start_Timestamp": "1400",
            "End_Timestamp": "1400",
            "Status": "0",
        },
        {
            "Kind": "process_range",
            "Operation": "roctxRangeStartA",
            "Message": "process range",
            "Process_Id": "100",
            "Thread_Id": "200",
            "Correlation_Id": "44",
            "Range_Id": "7",
            "Start_Timestamp": "1500",
            "End_Timestamp": "1650",
            "Status": "0",
        },
    ]


def test_roctx_long_utf8_message_conversion(rocprofv3, tmp_path):
    message = "π🙂range-" * 512
    encoded = message.encode("utf-8").hex()
    trace = tmp_path / "roctx-long.log"
    trace.write_text(
        "event=roctx_api phase=enter operation=roctxMarkA correlation_id=50 "
        f"process_id=100 thread_id=200 timestamp_ns=2000 message_hex={encoded}\n"
        "event=roctx_marker operation=roctxMarkA kind=mark phase=instant "
        "correlation_id=50 range_id=0 process_id=100 thread_id=200 "
        f"timestamp_ns=2000 status=0 message_hex={encoded}\n"
        "event=roctx_api phase=exit operation=roctxMarkA correlation_id=50 "
        "process_id=100 thread_id=200 timestamp_ns=2100 status=0\n",
        encoding="utf-8",
    )
    api_rows, graph_rows, marker_rows = rocprofv3.windows_api_trace_rows(trace)
    assert len(api_rows) == 1
    assert graph_rows == []
    assert len(marker_rows) == 1
    assert marker_rows[0]["Message"] == message


def test_hip_trace_rejects_unmatched_and_existing_output(rocprofv3, tmp_path, capsys):
    trace = tmp_path / "unmatched.log"
    trace.write_text(
        "event=hip_api phase=enter operation=hipMalloc correlation_id=9 "
        "process_id=1 thread_id=2 timestamp_ns=3\n",
        encoding="utf-8",
    )
    require_fatal(
        capsys,
        lambda: rocprofv3.windows_api_trace_rows(trace),
        "unmatched enter correlation IDs",
    )

    output = tmp_path / "hip.csv"
    output.write_text("retained", encoding="utf-8")
    retained = output.read_bytes()
    require_fatal(
        capsys,
        lambda: rocprofv3.windows_write_csv(
            output,
            [],
            ("Domain", "Function"),
        ),
        "output already exists",
    )
    assert output.read_bytes() == retained


def test_pid_output_collision_stops_suspended_target(rocprofv3, tmp_path, capsys):
    marker = tmp_path / "target-launched.txt"
    target = Path(os.environ["SystemRoot"]) / "System32" / "cmd.exe"
    retained = {}

    def reject_pid_output(process_id):
        output = tmp_path / f"result_{process_id}_kernel_trace.csv"
        output.write_text("retained", encoding="utf-8")
        retained["output"] = output
        rocprofv3.WindowsOutputTransaction().reserve([output])

    require_fatal(
        capsys,
        lambda: rocprofv3.windows_launch_in_job(
            [str(target), "/c", f"echo launched>{marker}"],
            dict(os.environ),
            tmp_path,
            reject_pid_output,
        ),
        "output already exists",
    )
    assert retained["output"].read_text(encoding="utf-8") == "retained"
    assert not marker.exists()
    assert not rocprofv3.windows_output_reservation_path(retained["output"]).exists()


def test_output_transaction_rolls_back_owned_files_only(rocprofv3, tmp_path, capsys):
    first = tmp_path / "first.csv"
    conflict = tmp_path / "conflict.csv"

    def publish_then_fail():
        with rocprofv3.WindowsOutputTransaction() as transaction:
            transaction.reserve([first, conflict])
            transaction.publish_csv(first, [{"value": 1}], ("value",))
            conflict.write_text("target-created\n", encoding="utf-8")
            transaction.publish_csv(conflict, [{"value": 2}], ("value",))

    require_fatal(capsys, publish_then_fail, "output already exists")
    assert not first.exists()
    assert conflict.read_text(encoding="utf-8") == "target-created\n"
    assert not rocprofv3.windows_output_reservation_path(first).exists()
    assert not rocprofv3.windows_output_reservation_path(conflict).exists()


def test_sdk_counter_environment_uses_common_contract(rocprofv3, tmp_path, monkeypatch):
    runtime_path = tmp_path / "runtime" / "bin"
    sdk_path = tmp_path / "sdk" / "bin" / "rocprofiler-sdk.dll"
    tool_path = tmp_path / "tool" / "bin" / "rocprofiler-sdk-tool.dll"
    monkeypatch.setattr(rocprofv3, "windows_core_bin", lambda: runtime_path)
    metrics_path = tmp_path / "sdk" / "share" / "rocprofiler-sdk" / "config.yaml"
    metrics_path.parent.mkdir(parents=True)
    metrics_path.write_text("metrics: []\n", encoding="utf-8")
    args = SimpleNamespace(
        kernel_include_regex="vector_add",
        kernel_exclude_regex="internal",
        kernel_iteration_range=["[2-3]"],
        truncate_kernels=True,
        mangled_kernels=True,
        selected_regions=True,
        selected_regions_ref_count=True,
        kernel_trace=True,
        stats=True,
    )
    environment = {
        "PATH": "inherited",
        "ROCPROF_COUNTER_GROUPS": "stale",
        "ROCPROF_OUTPUT_FILE_NAME": "inherited-name",
    }

    output_file = rocprofv3.windows_configure_sdk_counter_environment(
        environment,
        args,
        ["SQ_WAVES", "GRBM_COUNT"],
        ["csv", "json"],
        tmp_path / "output",
        None,
        sdk_path,
        tool_path,
    )

    assert output_file == "inherited-name"
    assert environment["PATH"].split(os.pathsep)[:3] == [
        str(runtime_path),
        str(sdk_path.parent),
        str(tool_path.parent),
    ]
    assert environment["ROCPROFILER_REGISTER_LIBRARY"] == str(sdk_path)
    assert environment["ROCP_TOOL_LIBRARIES"] == str(tool_path)
    assert "ROCPROFILER_LIBRARY_CTOR" not in environment
    assert environment["ROCPROF_COUNTERS"] == "pmc: SQ_WAVES GRBM_COUNT"
    assert "ROCPROF_COUNTER_GROUPS" not in environment
    assert environment["ROCPROF_OUTPUT_FORMAT"] == "csv,json"
    assert environment["ROCPROFILER_METRICS_PATH"] == str(metrics_path.parent)
    assert environment["ROCPROF_KERNEL_FILTER_INCLUDE_REGEX"] == "vector_add"
    assert environment["ROCPROF_KERNEL_FILTER_EXCLUDE_REGEX"] == "internal"
    assert environment["ROCPROF_KERNEL_FILTER_RANGE"] == "[2-3]"
    assert environment["ROCPROF_TRUNCATE_KERNELS"] == "1"
    assert environment["ROCPROF_DEMANGLE_KERNELS"] == "0"
    assert environment["ROCPROF_SELECTED_REGIONS"] == "1"
    assert environment["ROCPROF_SELECTED_REGIONS_REF_COUNT"] == "1"
    assert environment["ROCPROF_KERNEL_TRACE"] == "1"
    assert environment["ROCPROF_STATS"] == "1"

    outputs = rocprofv3.windows_sdk_output_paths(
        tmp_path / "output",
        "profile-%pid%",
        ["csv", "json", "rocpd"],
        4242,
        kernel_trace=True,
        stats=True,
    )
    assert [path.name for path in outputs] == [
        "profile-4242_agent_info.csv",
        "profile-4242_counter_collection.csv",
        "profile-4242_kernel_trace.csv",
        "profile-4242_kernel_stats.csv",
        "profile-4242_results.json",
        "profile-4242_results.db",
    ]
    nested_args = SimpleNamespace(
        output_directory=str(tmp_path / "directory with spaces" / "%pid%"),
        output_file="nested prefix/%pid%",
        sub_directory=None,
    )
    expected_base = (
        tmp_path / "directory with spaces" / "4242" / "nested prefix" / "4242"
    )
    assert rocprofv3.windows_kernel_output_path(nested_args, 4242) == (
        expected_base.with_name("4242_kernel_trace.csv")
    )
    assert rocprofv3.windows_kernel_stats_output_path(nested_args, 4242) == (
        expected_base.with_name("4242_kernel_stats.csv")
    )
    api_paths = rocprofv3.windows_api_trace_output_paths(
        nested_args, 4242, ("hip_api", "marker")
    )
    assert api_paths == {
        "hip_api": expected_base.with_name("4242_hip_api_trace.csv"),
        "marker": expected_base.with_name("4242_marker_trace.csv"),
    }
    nested_outputs = rocprofv3.windows_sdk_output_paths(
        nested_args.output_directory,
        nested_args.output_file,
        ["csv", "json"],
        4242,
        kernel_trace=True,
        stats=True,
    )
    assert all(path.parent == expected_base.parent for path in nested_outputs)
    assert [path.name for path in nested_outputs] == [
        "4242_agent_info.csv",
        "4242_counter_collection.csv",
        "4242_kernel_trace.csv",
        "4242_kernel_stats.csv",
        "4242_results.json",
    ]

    assert rocprofv3.windows_rocpd_sdk_formats(["rocpd"]) == ["json"]
    assert rocprofv3.windows_rocpd_sdk_formats(["csv", "rocpd"]) == [
        "csv",
        "json",
    ]


def write_rocpd_source(path, *, valid_metadata=True):
    symbol = {
        "size": 216,
        "kernel_id": 7,
        "code_object_id": 0,
        "kernel_name": "_Z15dispatch_vectorv",
        "kernel_object": 4096,
        "kernarg_segment_size": 32,
        "kernarg_segment_alignment": 16,
        "group_segment_size": 513,
        "private_segment_size": 64,
        "sgpr_count": 128 if valid_metadata else 0,
        "arch_vgpr_count": 32,
        "accum_vgpr_count": 0,
        "kernel_code_entry_byte_offset": 256,
        "kernel_address": {"handle": 4352},
        "formatted_kernel_name": "dispatch_vector()",
        "demangled_kernel_name": "dispatch_vector()",
        "truncated_kernel_name": "dispatch_vector",
    }
    dispatch_info = {
        "agent_id": {"handle": 222},
        "queue_id": {"handle": 2},
        "kernel_id": 7,
        "dispatch_id": 3,
        "private_segment_size": 64,
        "group_segment_size": 513,
        "workgroup_size": {"x": 256, "y": 1, "z": 1},
        "grid_size": {"x": 4096, "y": 1, "z": 1},
    }
    dispatch = {
        "thread_id": 44,
        "correlation_id": {"internal": 9, "external": 0},
        "start_timestamp": 1000,
        "end_timestamp": 1800,
        "dispatch_info": dispatch_info,
        "stream_id": {"handle": 0},
        "graph_exec_id": 0,
        "graph_node_id": 0,
    }
    counter = {
        "thread_id": 44,
        "dispatch_data": {
            "correlation_id": {"internal": 9, "external": 0},
            "start_timestamp": 1000,
            "end_timestamp": 1800,
            "dispatch_info": dispatch_info,
        },
        "records": [{"counter_id": {"handle": 333}, "value": 12.5}],
        "stream_id": {"handle": 0},
    }
    path.write_text(
        json.dumps(
            {
                "rocprofiler-sdk-tool": [
                    {
                        "metadata": {"pid": 4242, "init_time": 900, "fini_time": 1900},
                        "agents": [
                            {
                                "id": {"handle": 111},
                                "type": 1,
                                "node_id": 0,
                                "logical_node_id": 0,
                                "logical_node_type_id": 0,
                                "name": "CPU",
                            },
                            {
                                "id": {"handle": 222},
                                "type": 2,
                                "node_id": 1,
                                "logical_node_id": 1,
                                "logical_node_type_id": 0,
                                "name": "gfx1151",
                            },
                        ],
                        "counters": [
                            {
                                "agent_id": {"handle": 222},
                                "id": {"handle": 333},
                                "is_constant": 0,
                                "is_derived": 0,
                                "name": "SQ_WAVES",
                                "description": "waves",
                                "block": "SQ",
                                "expression": "",
                            }
                        ],
                        "kernel_symbols": [symbol],
                        "callback_records": {
                            "counter_collection": [counter],
                            "spm_counter_collection": [],
                        },
                        "buffer_records": {"kernel_dispatch": [dispatch]},
                    }
                ]
            }
        ),
        encoding="utf-8",
    )


def test_rocpd_conversion_uses_linux_schema_and_authoritative_records(
    rocprofv3, tmp_path
):
    source = tmp_path / "authoritative.json"
    database = tmp_path / "profile_results.db"
    write_rocpd_source(source)
    counts = rocprofv3.windows_generate_rocpd(
        source, database, ["target.exe", "--dispatches", "1"]
    )
    assert counts == {"dispatches": 1, "counters": 1, "kernel_symbols": 1}

    with sqlite3.connect(database) as connection:
        assert connection.execute("PRAGMA integrity_check").fetchone() == ("ok",)
        assert connection.execute("PRAGMA foreign_key_check").fetchall() == []
        assert connection.execute(
            "SELECT value FROM rocpd_metadata WHERE tag = 'schema_version'"
        ).fetchone() == ("3.0.3",)
        assert connection.execute(
            "SELECT dispatch_id, name, start, end, duration, lds_size, scratch_size, "
            "vgpr_count, accum_vgpr_count, sgpr_count FROM kernels"
        ).fetchone() == (
            3,
            "dispatch_vector()",
            1000,
            1800,
            800,
            1024,
            64,
            32,
            0,
            128,
        )
        assert connection.execute(
            "SELECT dispatch_id, counter_name, counter_value FROM pmc_events"
        ).fetchone() == (3, "SQ_WAVES", 12.5)
        assert connection.execute(
            "SELECT name, total_calls, total_duration, average, percentage "
            "FROM top_kernels"
        ).fetchone() == ("dispatch_vector()", 1, 0.8, 0.8, 100.0)
        assert connection.execute(
            "SELECT command FROM rocpd_info_process"
        ).fetchone() == ('target.exe --dispatches 1',)


def test_rocpd_conversion_failure_removes_partial_database(
    rocprofv3, tmp_path, capsys
):
    source = tmp_path / "invalid.json"
    database = tmp_path / "invalid_results.db"
    write_rocpd_source(source, valid_metadata=False)
    require_fatal(
        capsys,
        lambda: rocprofv3.windows_generate_rocpd(source, database, ["target.exe"]),
        "rocpd_conversion_failed: kernel_metadata_missing",
    )
    assert not database.exists()
    assert not list(tmp_path.glob(".*.tmp"))


def test_availability_initialization_failure_is_reported(tmp_path, monkeypatch, capsys):
    class StatusFunction:
        def __call__(self, message):
            pointer = ctypes.cast(message, ctypes.POINTER(ctypes.c_char_p))
            pointer[0] = b"counter metadata query failed"
            return 9

    class Library:
        availability_status = StatusFunction()

    availability = load_availability()
    availability.loadLibrary.c_lib = None
    availability.loadLibrary.libname = str(tmp_path / "availability.dll")
    monkeypatch.setattr(availability.ctypes, "CDLL", lambda _: Library())
    require_fatal(
        capsys,
        availability.get_library,
        "Availability initialization failed (9): counter metadata query failed",
    )
    assert availability.loadLibrary.c_lib is None


def test_sdk_result_status_distinguishes_profiler_failures(rocprofv3, tmp_path, capsys):
    result = tmp_path / "result.txt"
    result.write_text("version=1\nstatus=success_records\ndetail=\n", encoding="utf-8")
    output = tmp_path / "counter_collection.csv"
    output.write_text("records\n", encoding="utf-8")
    assert rocprofv3.windows_sdk_result(result, 7, [output]) == (
        "success_records",
        7,
    )
    require_fatal(
        capsys,
        lambda: rocprofv3.windows_sdk_result(
            result, 0, [tmp_path / "missing.csv"]
        ),
        "did not publish",
    )

    result.write_text(
        "version=1\nstatus=output_publication_failed\ndetail=could not write output\n",
        encoding="utf-8",
    )
    require_fatal(
        capsys,
        lambda: rocprofv3.windows_sdk_result(result, 0),
        "output_publication_failed",
    )

    result.unlink()
    assert rocprofv3.windows_sdk_result(result, 0) == ("success_no_dispatch", 0)
    assert "status=success_no_dispatch" in result.read_text(encoding="utf-8")


def test_sdk_counter_values_accept_one_normalized_pass(rocprofv3, capsys):
    assert rocprofv3.windows_sdk_counter_values(
        SimpleNamespace(pmc=[["SQ_WAVES", "GRBM_COUNT"]])
    ) == ["SQ_WAVES", "GRBM_COUNT"]
    require_fatal(
        capsys,
        lambda: rocprofv3.windows_sdk_counter_values(
            SimpleNamespace(pmc=[["SQ_WAVES"], ["GRBM_COUNT"]])
        ),
        "multiple counter groups",
    )
