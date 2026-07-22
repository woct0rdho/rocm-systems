from __future__ import annotations

import csv
import json
import math
import os
from pathlib import Path
import sqlite3
import subprocess
import sys


COMMON = Path(__file__).resolve().parent.parent / "common"
if str(COMMON) not in sys.path:
    sys.path.insert(0, str(COMMON))

from process_cleanup import assert_target_stopped


def run_cli(
    *arguments: str, timeout: int = 120, environment_updates: dict[str, str] | None = None
):
    python = Path(os.environ["ROCPROFV3_TEST_PYTHON"]).resolve()
    script = Path(os.environ["ROCPROFV3_TEST_BUILT_SCRIPT"]).resolve()
    environment = dict(os.environ)
    for name in (
        "HSA_TOOLS_LIB",
        "ROCR_USE_PM4",
        "WSLKMT_VENDOR_PACKET",
        "ROCPROF_COUNTERS",
        "ROCPROF_COUNTER_GROUPS",
        "ROCPROF_OUTPUT_PATH",
        "ROCPROF_OUTPUT_FILE_NAME",
    ):
        environment.pop(name, None)
    if environment_updates:
        environment.update(environment_updates)
    result = subprocess.run(
        [str(python), str(script), *arguments],
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
    )
    assert_target_stopped(workload())
    return result


def read_csv(path: Path):
    with path.open(encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))


def workload():
    return Path(os.environ["ROCPROFV3_TEST_WORKLOAD"]).resolve()


def assert_kernel_stats(kernel_rows, stats_rows):
    samples = {}
    for row in kernel_rows:
        duration = int(row["End_Timestamp"]) - int(row["Start_Timestamp"])
        assert duration > 0
        samples.setdefault(row["Kernel_Name"], []).append(duration)
    total = sum(sum(values) for values in samples.values())
    assert {row["Name"] for row in stats_rows} == set(samples)
    for row in stats_rows:
        values = samples[row["Name"]]
        duration_sum = sum(values)
        count = len(values)
        variance = 0.0
        if count > 1:
            variance = (
                sum(value * value for value in values)
                - (duration_sum * duration_sum) / count
            ) / (count - 1)
        assert int(row["Calls"]) == count
        assert int(row["TotalDurationNs"]) == duration_sum
        assert math.isclose(
            float(row["AverageNs"]), duration_sum / count, rel_tol=1.0e-6
        )
        assert int(row["MinNs"]) == min(values)
        assert int(row["MaxNs"]) == max(values)
        assert math.isclose(
            float(row["Percentage"]),
            (duration_sum / total) * 100.0,
            rel_tol=1.0e-3,
            abs_tol=1.0e-2,
        )
        assert math.isclose(
            float(row["StdDev"]),
            math.sqrt(abs(variance)),
            rel_tol=1.0e-6,
            abs_tol=1.0e-6,
        )


def assert_kernel_counter_join(counter_rows, kernel_rows):
    counter_by_dispatch = {}
    for row in counter_rows:
        counter_by_dispatch.setdefault(int(row["Dispatch_Id"]), row)
    kernel_by_dispatch = {int(row["Dispatch_Id"]): row for row in kernel_rows}
    assert set(kernel_by_dispatch) == set(counter_by_dispatch)
    assert len(kernel_by_dispatch) == len(kernel_rows)
    for dispatch_id, kernel in kernel_by_dispatch.items():
        counter = counter_by_dispatch[dispatch_id]
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
            "LDS_Block_Size",
            "Scratch_Size",
            "VGPR_Count",
            "Accum_VGPR_Count",
            "SGPR_Count",
        ):
            assert kernel[field] == counter[field]


def cmd_exe():
    return Path(os.environ["SystemRoot"]) / "System32" / "cmd.exe"


def test_output_key_contract_matches_native_producer(tmp_path):
    output_root = tmp_path / "output root" / "%tag%" / "%pid%" / "%env{PATH_TOKEN}%"
    result = run_cli(
        "--pmc",
        "SQ_WAVES",
        "--mpi-world-rank-variable",
        "CUSTOM_RANK",
        "--mpi-world-size-variable",
        "CUSTOM_SIZE",
        "-d",
        str(output_root),
        "-o",
        "prefix/%arg1%_%rank%_%p",
        "--",
        str(workload()),
        "--dispatches",
        "1",
        environment_updates={
            "CUSTOM_RANK": "4",
            "CUSTOM_SIZE": "8",
            "SLURM_JOB_ID": "77",
            "PATH_TOKEN": "nested value",
        },
    )
    assert result.returncode == 0, result.stdout
    outputs = list((tmp_path / "output root").rglob("*_counter_collection.csv"))
    assert len(outputs) == 1
    rows = read_csv(outputs[0])
    assert len(rows) == 1
    process_id = rows[0]["Process_Id"]
    relative = outputs[0].relative_to(tmp_path / "output root")
    assert relative.parts[:3] == (workload().name, process_id, "nested_value")
    assert relative.parts[3] == "prefix"
    assert relative.name == (
        f"--dispatches_4_{process_id}_counter_collection.csv"
    )


def test_inherited_output_name_supports_recursion_and_spaces(tmp_path):
    result = run_cli(
        "--pmc",
        "SQ_WAVES",
        "-d",
        str(tmp_path / "inherited root" / "%pid%"),
        "--",
        str(workload()),
        "--dispatches",
        "1",
        environment_updates={
            "ROCPROF_OUTPUT_FILE_NAME": "%env{OUTPUT_PREFIX}%/{pid}",
            "OUTPUT_PREFIX": "inherited name",
        },
    )
    assert result.returncode == 0, result.stdout
    outputs = list((tmp_path / "inherited root").rglob("*_counter_collection.csv"))
    assert len(outputs) == 1
    rows = read_csv(outputs[0])
    process_id = rows[0]["Process_Id"]
    assert outputs[0].relative_to(tmp_path / "inherited root").parts == (
        process_id,
        "inherited_name",
        f"{process_id}_counter_collection.csv",
    )


def test_ordinary_csv_json_and_filtering(tmp_path):
    result = run_cli(
        "--pmc",
        "SQ_WAVES",
        "--kernel-include-regex",
        "vector_add",
        "--kernel-iteration-range",
        "[2-3]",
        "-f",
        "csv",
        "json",
        "-d",
        str(tmp_path),
        "-o",
        "filtered",
        "--",
        str(workload()),
    )
    assert result.returncode == 0, result.stdout

    rows = read_csv(tmp_path / "filtered_counter_collection.csv")
    assert [int(row["Dispatch_Id"]) for row in rows] == [2, 3]
    assert {row["Counter_Name"] for row in rows} == {"SQ_WAVES"}
    assert all(float(row["Counter_Value"]) > 0 for row in rows)
    assert all(int(row["Start_Timestamp"]) > 0 for row in rows)
    assert all(int(row["End_Timestamp"]) > int(row["Start_Timestamp"]) for row in rows)
    assert all(row["Kernel_Name"].startswith("vector_add(") for row in rows)

    document = json.loads((tmp_path / "filtered_results.json").read_text("utf-8"))
    root = document["rocprofiler-sdk-tool"][0]
    assert len(root["callback_records"]["counter_collection"]) == 2
    assert root["buffer_records"]["kernel_dispatch"] == []
    assert len(root["agents"]) == 2


def test_counter_and_kernel_trace_use_one_dispatch_record(tmp_path):
    result = run_cli(
        "--kernel-trace",
        "--stats",
        "--pmc",
        "SQ_WAVES",
        "--kernel-include-regex",
        "vector_add",
        "--kernel-iteration-range",
        "[2-3]",
        "-f",
        "csv",
        "json",
        "-d",
        str(tmp_path),
        "-o",
        "composed-kernel",
        "--",
        str(workload()),
    )
    assert result.returncode == 0, result.stdout

    counter_rows = read_csv(tmp_path / "composed-kernel_counter_collection.csv")
    kernel_rows = read_csv(tmp_path / "composed-kernel_kernel_trace.csv")
    stats_rows = read_csv(tmp_path / "composed-kernel_kernel_stats.csv")
    assert len(counter_rows) == len(kernel_rows) == 2
    assert_kernel_counter_join(counter_rows, kernel_rows)
    assert_kernel_stats(kernel_rows, stats_rows)
    assert all(row["Kind"] == "KERNEL_DISPATCH" for row in kernel_rows)

    document = json.loads(
        (tmp_path / "composed-kernel_results.json").read_text("utf-8")
    )["rocprofiler-sdk-tool"][0]
    json_kernel = document["buffer_records"]["kernel_dispatch"]
    json_counter = document["callback_records"]["counter_collection"]
    json_summary = document["summary"]
    assert len(json_kernel) == len(json_counter) == 2
    assert len(json_summary) == 1
    assert json_summary[0]["domain"] == "KERNEL_DISPATCH"
    assert json_summary[0]["stats"]["count"] == 2
    assert [entry["key"] for entry in json_summary[0]["stats"]["operations"]] == [
        kernel_rows[0]["Kernel_Name"]
    ]
    assert [record["dispatch_info"]["dispatch_id"] for record in json_kernel] == [
        record["dispatch_data"]["dispatch_info"]["dispatch_id"]
        for record in json_counter
    ]
    for kernel, counter in zip(json_kernel, json_counter):
        dispatch = counter["dispatch_data"]
        assert kernel["dispatch_info"] == dispatch["dispatch_info"]
        assert kernel["correlation_id"] == dispatch["correlation_id"]
        assert kernel["thread_id"] == counter["thread_id"]
        assert kernel["start_timestamp"] == dispatch["start_timestamp"]
        assert kernel["end_timestamp"] == dispatch["end_timestamp"]


def test_rocpd_database_uses_authoritative_dispatch_and_counter_records(tmp_path):
    result = run_cli(
        "--stats",
        "--pmc",
        "SQ_WAVES",
        "-f",
        "rocpd",
        "-d",
        str(tmp_path),
        "-o",
        "dispatch-database",
        "--",
        str(workload()),
    )
    assert result.returncode == 0, result.stdout
    assert "Windows ROCpd: dispatches=8" in result.stdout
    assert "kernel_symbols=1" in result.stdout

    database = tmp_path / "dispatch-database_results.db"
    assert database.is_file()
    assert not (tmp_path / "dispatch-database_results.json").exists()
    assert not (tmp_path / "dispatch-database_counter_collection.csv").exists()
    with sqlite3.connect(database) as connection:
        assert connection.execute("PRAGMA integrity_check").fetchone() == ("ok",)
        assert connection.execute("PRAGMA foreign_key_check").fetchall() == []
        assert connection.execute(
            "SELECT value FROM rocpd_metadata WHERE tag = 'schema_version'"
        ).fetchone() == ("3.0.3",)
        kernels = connection.execute(
            "SELECT dispatch_id, name, start, end, duration, lds_size, scratch_size, "
            "vgpr_count, accum_vgpr_count, sgpr_count FROM kernels ORDER BY dispatch_id"
        ).fetchall()
        assert len(kernels) == 8
        assert [row[0] for row in kernels] == list(range(1, 9))
        assert all("vector_add" in row[1] for row in kernels)
        assert all(
            row[2] > 0 and row[3] > row[2] and row[4] == row[3] - row[2]
            for row in kernels
        )
        assert all(
            row[5] >= 0
            and row[6] >= 0
            and row[7] > 0
            and row[8] >= 0
            and row[9] > 0
            for row in kernels
        )
        pmc_events = connection.execute(
            "SELECT dispatch_id, counter_name, counter_value FROM pmc_events "
            "ORDER BY dispatch_id"
        ).fetchall()
        assert len(pmc_events) >= 8
        assert {row[0] for row in pmc_events} == set(range(1, 9))
        assert {row[1] for row in pmc_events} == {"SQ_WAVES"}
        values_by_dispatch = {}
        for dispatch_id, _, value in pmc_events:
            values_by_dispatch.setdefault(dispatch_id, []).append(value)
        assert len({len(values) for values in values_by_dispatch.values()}) == 1
        assert all(sum(values) > 0 for values in values_by_dispatch.values())
        top_kernel = connection.execute(
            "SELECT name, total_calls, total_duration, average, percentage "
            "FROM top_kernels"
        ).fetchone()
        assert "vector_add" in top_kernel[0]
        assert top_kernel[1] == 8
        assert top_kernel[2] > 0 and top_kernel[3] > 0
        assert math.isclose(top_kernel[4], 100.0)


def test_multiple_pmc_groups_publish_separate_passes(tmp_path):
    result = run_cli(
        "--kernel-trace",
        "--stats",
        "--pmc",
        "SQ_WAVES",
        "--pmc",
        "GRBM_COUNT",
        "-d",
        str(tmp_path / "multipass root" / "%pid%"),
        "-o",
        "nested/multi",
        "--",
        str(workload()),
    )
    assert result.returncode == 0, result.stdout

    root = tmp_path / "multipass root"
    first_path = next(root.glob("*/pass_1/nested/multi_counter_collection.csv"))
    second_path = next(root.glob("*/pass_2/nested/multi_counter_collection.csv"))
    first_root = first_path.parents[2]
    second_root = second_path.parents[2]
    first = read_csv(first_path)
    second = read_csv(second_path)
    first_kernel = read_csv(first_root / "pass_1/nested/multi_kernel_trace.csv")
    second_kernel = read_csv(second_root / "pass_2/nested/multi_kernel_trace.csv")
    first_stats = read_csv(first_root / "pass_1/nested/multi_kernel_stats.csv")
    second_stats = read_csv(second_root / "pass_2/nested/multi_kernel_stats.csv")
    assert first_root.name == first[0]["Process_Id"]
    assert second_root.name == second[0]["Process_Id"]
    assert len(first) == len(second) == 8
    assert len(first_kernel) == len(second_kernel) == 8
    assert {row["Counter_Name"] for row in first} == {"SQ_WAVES"}
    assert {row["Counter_Name"] for row in second} == {"GRBM_COUNT"}
    assert sum(float(row["Counter_Value"]) for row in first) > 0
    assert sum(float(row["Counter_Value"]) for row in second) > 0
    assert_kernel_counter_join(first, first_kernel)
    assert_kernel_counter_join(second, second_kernel)
    assert_kernel_stats(first_kernel, first_stats)
    assert_kernel_stats(second_kernel, second_stats)


def test_multiple_pmc_groups_publish_process_local_rocpd_databases(tmp_path):
    result = run_cli(
        "--kernel-trace",
        "--pmc",
        "SQ_WAVES",
        "--pmc",
        "GRBM_COUNT",
        "-f",
        "rocpd",
        "-d",
        str(tmp_path),
        "-o",
        "multi-database",
        "--",
        str(workload()),
        "--dispatches",
        "2",
    )
    assert result.returncode == 0, result.stdout
    assert result.stdout.count("Windows ROCpd: dispatches=2") == 2

    for pass_id, counter_name in ((1, "SQ_WAVES"), (2, "GRBM_COUNT")):
        output = tmp_path / f"pass_{pass_id}"
        database = output / "multi-database_results.db"
        assert database.is_file()
        assert not (output / "multi-database_results.json").exists()
        with sqlite3.connect(database) as connection:
            assert [
                row[0]
                for row in connection.execute(
                    "SELECT dispatch_id FROM kernels ORDER BY dispatch_id"
                )
            ] == [1, 2]
            assert {
                row[0]
                for row in connection.execute(
                    "SELECT DISTINCT counter_name FROM pmc_events"
                )
            } == {counter_name}
            assert connection.execute(
                "SELECT value FROM rocpd_metadata WHERE tag = 'dispatch_count'"
            ).fetchone() == ("2",)


def test_selected_regions_and_reference_counting(tmp_path):
    inactive = tmp_path / "inactive"
    result = run_cli(
        "--pmc",
        "SQ_WAVES",
        "--selected-regions",
        "-d",
        str(inactive),
        "-o",
        "inactive",
        "--",
        str(workload()),
    )
    assert result.returncode == 0, result.stdout
    assert not list(inactive.glob("*_counter_collection.csv"))

    no_ref = tmp_path / "no-ref"
    result = run_cli(
        "--pmc",
        "SQ_WAVES",
        "--selected-regions",
        "-d",
        str(no_ref),
        "-o",
        "selected",
        "--",
        str(workload()),
        "--nested-selected-regions",
    )
    assert result.returncode == 0, result.stdout
    assert len(read_csv(no_ref / "selected_counter_collection.csv")) == 4

    with_ref = tmp_path / "with-ref"
    result = run_cli(
        "--pmc",
        "SQ_WAVES",
        "--selected-regions",
        "--selected-regions-ref-count",
        "-d",
        str(with_ref),
        "-o",
        "selected",
        "--",
        str(workload()),
        "--nested-selected-regions",
    )
    assert result.returncode == 0, result.stdout
    assert len(read_csv(with_ref / "selected_counter_collection.csv")) == 8


def test_counter_and_hip_trace_compose_in_one_process(tmp_path):
    result = run_cli(
        "--pmc",
        "SQ_WAVES",
        "--hip-runtime-trace",
        "--kernel-iteration-range",
        "[2-3]",
        "-f",
        "csv",
        "json",
        "-d",
        str(tmp_path / "composed root" / "%pid%"),
        "-o",
        "nested/composed-%p",
        "--",
        str(workload()),
    )
    assert result.returncode == 0, result.stdout
    counter_path = next(
        (tmp_path / "composed root").rglob("*_counter_collection.csv")
    )
    counter_rows = read_csv(counter_path)
    assert len(counter_rows) == 2
    process_id = counter_rows[0]["Process_Id"]
    output = tmp_path / "composed root" / process_id / "nested"
    prefix = output / f"composed-{process_id}"
    assert counter_path == prefix.with_name(
        f"composed-{process_id}_counter_collection.csv"
    )
    document = json.loads(
        prefix.with_name(f"composed-{process_id}_results.json").read_text("utf-8")
    )
    assert (
        len(
            document["rocprofiler-sdk-tool"][0]["callback_records"][
                "counter_collection"
            ]
        )
        == 2
    )
    hip_rows = read_csv(
        prefix.with_name(f"composed-{process_id}_hip_api_trace.csv")
    )
    assert hip_rows
    assert {row["Domain"] for row in hip_rows} == {"HIP_RUNTIME_API"}


def test_explicit_graph_disable_ignores_unrelated_graph_output(tmp_path):
    graph_output = tmp_path / "disabled_hip_graph_trace.csv"
    graph_output.write_text("retained\n", encoding="utf-8")
    result = run_cli(
        "--hip-runtime-trace",
        "--hip-graph-trace=false",
        "-d",
        str(tmp_path),
        "-o",
        "disabled",
        "--",
        str(workload()),
        "--graph",
        "--dispatches",
        "2",
    )
    assert result.returncode == 0, result.stdout
    assert graph_output.read_text(encoding="utf-8") == "retained\n"
    assert read_csv(tmp_path / "disabled_hip_api_trace.csv")
    assert not list(tmp_path.glob(".*.rocprofv3-reserve"))


def test_api_publication_failure_rolls_back_earlier_trace(tmp_path):
    conflict = tmp_path / "api-failure_marker_api_trace.csv"
    result = run_cli(
        "--hip-runtime-trace",
        "--marker-trace",
        "-d",
        str(tmp_path),
        "-o",
        "api-failure",
        "--",
        str(workload()),
        "--markers",
        "--dispatches",
        "2",
        "--create-file",
        str(conflict),
    )
    assert result.returncode == 1
    assert "output already exists" in result.stdout
    assert conflict.read_text(encoding="utf-8") == "target-created\n"
    assert not (tmp_path / "api-failure_hip_api_trace.csv").exists()
    assert not (tmp_path / "api-failure_marker_trace.csv").exists()
    assert not list(tmp_path.glob(".*.rocprofv3-reserve"))


def test_trace_failure_rolls_back_native_counter_outputs(tmp_path):
    conflict = tmp_path / "composed-failure_hip_api_trace.csv"
    result = run_cli(
        "--pmc",
        "SQ_WAVES",
        "--hip-runtime-trace",
        "-f",
        "csv",
        "json",
        "-d",
        str(tmp_path),
        "-o",
        "composed-failure",
        "--",
        str(workload()),
        "--dispatches",
        "2",
        "--create-file",
        str(conflict),
    )
    assert result.returncode == 1
    assert "output already exists" in result.stdout
    assert conflict.read_text(encoding="utf-8") == "target-created\n"
    assert not (tmp_path / "composed-failure_agent_info.csv").exists()
    assert not (tmp_path / "composed-failure_counter_collection.csv").exists()
    assert not (tmp_path / "composed-failure_results.json").exists()
    assert not list(tmp_path.glob(".*.rocprofv3-reserve"))


def test_unknown_counter_warns_without_output(tmp_path):
    result = run_cli(
        "--pmc",
        "NO_SUCH_COUNTER",
        "-d",
        str(tmp_path),
        "-o",
        "unknown",
        "--",
        str(workload()),
    )
    assert result.returncode == 0, result.stdout
    assert "Unable to find counter" in result.stdout
    assert not list(tmp_path.glob("unknown_*"))


def test_no_dispatch_and_target_failure_preserve_status(tmp_path):
    no_dispatch = tmp_path / "no-dispatch"
    result = run_cli(
        "--pmc",
        "SQ_WAVES",
        "--stats",
        "-d",
        str(no_dispatch),
        "-o",
        "none",
        "--",
        str(cmd_exe()),
        "/d",
        "/c",
        "exit 0",
    )
    assert result.returncode == 0, result.stdout
    assert not list(no_dispatch.glob("none_*"))

    failed = tmp_path / "failed"
    result = run_cli(
        "--pmc",
        "SQ_WAVES",
        "-d",
        str(failed),
        "-o",
        "failed",
        "--",
        str(cmd_exe()),
        "/d",
        "/c",
        "exit 7",
    )
    assert result.returncode == 7, result.stdout
    assert not list(failed.glob("failed_*"))


def test_repeated_fresh_processes_drain_counter_callbacks(tmp_path):
    for iteration in range(10):
        output = tmp_path / f"repeat-{iteration + 1}"
        result = run_cli(
            "--pmc",
            "SQ_WAVES",
            "-f",
            "csv",
            "-d",
            str(output),
            "-o",
            "repeat",
            "--",
            str(workload()),
            "--dispatches",
            "2",
        )
        assert result.returncode == 0, result.stdout
        rows = read_csv(output / "repeat_counter_collection.csv")
        assert len(rows) == 2
        assert [int(row["Dispatch_Id"]) for row in rows] == [1, 2]
        assert {row["Counter_Name"] for row in rows} == {"SQ_WAVES"}
        assert all(float(row["Counter_Value"]) > 0 for row in rows)
        assert all(
            int(row["Start_Timestamp"]) > 0 for row in rows
        ), result.stdout
        assert all(
            int(row["End_Timestamp"]) > int(row["Start_Timestamp"]) for row in rows
        ), result.stdout


def test_output_publication_failure_is_reported_and_partial_output_is_removed(tmp_path):
    conflict = tmp_path / "conflict_counter_collection.csv"
    result = run_cli(
        "--pmc",
        "SQ_WAVES",
        "-d",
        str(tmp_path),
        "-o",
        "conflict",
        "--",
        str(workload()),
        "--dispatches",
        "2",
        "--create-file",
        str(conflict),
    )
    assert result.returncode == 1
    assert "output_publication_failed" in result.stdout
    assert conflict.read_text(encoding="utf-8") == "target-created\n"
    assert not (tmp_path / "conflict_agent_info.csv").exists()


def test_kernel_trace_publication_failure_preserves_target_file(tmp_path):
    conflict = tmp_path / "trace-conflict_kernel_trace.csv"
    result = run_cli(
        "--kernel-trace",
        "--pmc",
        "SQ_WAVES",
        "-d",
        str(tmp_path),
        "-o",
        "trace-conflict",
        "--",
        str(workload()),
        "--dispatches",
        "2",
        "--create-file",
        str(conflict),
    )
    assert result.returncode == 1
    assert "output_publication_failed" in result.stdout
    assert conflict.read_text(encoding="utf-8") == "target-created\n"
    assert not (tmp_path / "trace-conflict_agent_info.csv").exists()
    assert not (tmp_path / "trace-conflict_counter_collection.csv").exists()


def test_kernel_stats_publication_failure_preserves_target_file(tmp_path):
    conflict = tmp_path / "stats-conflict_kernel_stats.csv"
    result = run_cli(
        "--kernel-trace",
        "--stats",
        "--pmc",
        "SQ_WAVES",
        "-d",
        str(tmp_path),
        "-o",
        "stats-conflict",
        "--",
        str(workload()),
        "--dispatches",
        "2",
        "--create-file",
        str(conflict),
    )
    assert result.returncode == 1
    assert "output_publication_failed" in result.stdout
    assert conflict.read_text(encoding="utf-8") == "target-created\n"
    assert not (tmp_path / "stats-conflict_agent_info.csv").exists()
    assert not (tmp_path / "stats-conflict_counter_collection.csv").exists()
    assert not (tmp_path / "stats-conflict_kernel_trace.csv").exists()


def test_rocpd_target_conflict_is_preserved_and_internal_json_is_removed(tmp_path):
    conflict = tmp_path / "database-conflict_results.db"
    result = run_cli(
        "--kernel-trace",
        "--pmc",
        "SQ_WAVES",
        "-f",
        "csv",
        "rocpd",
        "-d",
        str(tmp_path),
        "-o",
        "database-conflict",
        "--",
        str(workload()),
        "--dispatches",
        "2",
        "--create-file",
        str(conflict),
    )
    assert result.returncode == 1
    assert "rocpd_conversion_failed: output already exists" in result.stdout
    assert conflict.read_text(encoding="utf-8") == "target-created\n"
    assert not (tmp_path / "database-conflict_agent_info.csv").exists()
    assert not (tmp_path / "database-conflict_counter_collection.csv").exists()
    assert not (tmp_path / "database-conflict_results.json").exists()
    assert not list(tmp_path.glob(".*.rocprofv3-reserve"))


def test_existing_rocpd_output_prevents_launch(tmp_path):
    output = tmp_path / "retained-results_results.db"
    output.write_text("retained\n", encoding="utf-8")
    marker = tmp_path / "launched.txt"
    result = run_cli(
        "--pmc",
        "SQ_WAVES",
        "-f",
        "rocpd",
        "-d",
        str(tmp_path),
        "-o",
        "retained-results",
        "--",
        str(cmd_exe()),
        "/d",
        "/c",
        f"echo launched>{marker}",
    )
    assert result.returncode == 1
    assert "output already exists" in result.stdout
    assert output.read_text(encoding="utf-8") == "retained\n"
    assert not marker.exists()


def test_existing_counter_output_prevents_launch(tmp_path):
    output = tmp_path / "retained_counter_collection.csv"
    output.write_text("retained\n", encoding="utf-8")
    marker = tmp_path / "launched.txt"
    result = run_cli(
        "--pmc",
        "SQ_WAVES",
        "-d",
        str(tmp_path),
        "-o",
        "retained",
        "--",
        str(cmd_exe()),
        "/d",
        "/c",
        f"echo launched>{marker}",
    )
    assert result.returncode == 1
    assert "output already exists" in result.stdout
    assert output.read_text(encoding="utf-8") == "retained\n"
    assert not marker.exists()
