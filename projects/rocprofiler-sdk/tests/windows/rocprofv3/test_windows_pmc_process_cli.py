from __future__ import annotations

import csv
import json
import os
from pathlib import Path
import subprocess
import sys


COMMON = Path(__file__).resolve().parent.parent / "common"
if str(COMMON) not in sys.path:
    sys.path.insert(0, str(COMMON))

from process_cleanup import assert_target_stopped


def run_cli(*arguments: str, timeout: int = 120):
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


def cmd_exe():
    return Path(os.environ["SystemRoot"]) / "System32" / "cmd.exe"


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
    assert all(
        int(row["End_Timestamp"]) > int(row["Start_Timestamp"]) for row in rows
    )
    assert all(row["Kernel_Name"].startswith("vector_add(") for row in rows)

    document = json.loads((tmp_path / "filtered_results.json").read_text("utf-8"))
    root = document["rocprofiler-sdk-tool"][0]
    assert len(root["callback_records"]["counter_collection"]) == 2
    assert len(root["agents"]) == 2


def test_multiple_pmc_groups_publish_separate_passes(tmp_path):
    result = run_cli(
        "--pmc",
        "SQ_WAVES",
        "--pmc",
        "GRBM_COUNT",
        "-d",
        str(tmp_path),
        "-o",
        "multi",
        "--",
        str(workload()),
    )
    assert result.returncode == 0, result.stdout

    first = read_csv(tmp_path / "pass_1" / "multi_counter_collection.csv")
    second = read_csv(tmp_path / "pass_2" / "multi_counter_collection.csv")
    assert len(first) == len(second) == 8
    assert {row["Counter_Name"] for row in first} == {"SQ_WAVES"}
    assert {row["Counter_Name"] for row in second} == {"GRBM_COUNT"}
    assert sum(float(row["Counter_Value"]) for row in first) > 0
    assert sum(float(row["Counter_Value"]) for row in second) > 0


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
        str(tmp_path),
        "-o",
        "composed",
        "--",
        str(workload()),
    )
    assert result.returncode == 0, result.stdout
    assert len(read_csv(tmp_path / "composed_counter_collection.csv")) == 2
    document = json.loads((tmp_path / "composed_results.json").read_text("utf-8"))
    assert len(
        document["rocprofiler-sdk-tool"][0]["callback_records"][
            "counter_collection"
        ]
    ) == 2
    hip_rows = read_csv(tmp_path / "composed_hip_api_trace.csv")
    assert hip_rows
    assert {row["Domain"] for row in hip_rows} == {"HIP_RUNTIME_API"}


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
        assert all(int(row["Start_Timestamp"]) > 0 for row in rows)
        assert all(
            int(row["End_Timestamp"]) > int(row["Start_Timestamp"])
            for row in rows
        )


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
