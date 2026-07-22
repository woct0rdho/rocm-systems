from __future__ import annotations

import json
import math
import os
import re
import sys
from collections import Counter
from pathlib import Path


PMC_PARITY_DIRECTORY = Path(__file__).resolve().parents[2] / "pmc-parity"
PMC_CONTRACT = json.loads(
    (PMC_PARITY_DIRECTORY / "gfx1151_linux_contract.json").read_text(encoding="utf-8")
)
sys.path.insert(0, str(PMC_PARITY_DIRECTORY))
from pmc_contract import parse_counter_info  # noqa: E402


def load_result():
    path = Path(os.environ["ROCPROFILER_WINDOWS_INTEGRATION_RESULT"])
    return json.loads(path.read_text(encoding="utf-8"))


def require_workload(output: str):
    for marker in (
        "architecture=gfx1151",
        "dispatches=8",
        "work_items=1048576",
        "workgroup_size=256",
        "kernel=vector_add",
        "resource=loaded",
        "validation=passed",
    ):
        assert marker in output


def require_original_target(data):
    modules = re.search(
        r"(?:^|\n)executable=(.+?) runtime=(.+?) resource=loaded", data["stdout"]
    )
    assert modules
    target = Path(data["target"]).resolve()
    assert Path(modules.group(1)).resolve() == target
    assert Path(modules.group(2)).resolve() == target.with_name("amdhip64_7.dll")


def require_agent_field(output: str, name: str, value: str):
    fields = {}
    for line in output.splitlines():
        if ":" not in line:
            continue
        field, field_value = line.split(":", 1)
        fields[field.strip()] = field_value.strip()
    assert fields[name] == value


def test_windows_integration_case():
    result = load_result()
    case = os.environ["ROCPROFILER_WINDOWS_INTEGRATION_CASE"]
    assert result["case"] == case
    data = result["data"]

    if case == "availability":
        for name, command in data.items():
            expected_returncode = (
                1 if name in {"pmc_check_rejected", "pmc_check_unknown"} else 0
            )
            assert command["returncode"] == expected_returncode

        for command_name in ("list", "agent", "catalog"):
            output = data[command_name]["stdout"]
            assert "Invalid metric" not in output
            assert "absl::InitializeLog" not in output
            assert not re.search(r"^E\d{4} ", output, re.MULTILINE)

        list_output = data["list"]["stdout"]
        for marker in (
            "gfx1151",
            "GRBM_COUNT",
            "GRBM_GUI_ACTIVE",
            "SQ_WAVES",
            "TA_TA_BUSY",
            "TCP_REQ",
            "GL1C_BUSY",
            "GL2C_HIT",
            "GDSInsts",
            "DIMENSION_WGP[0:4]",
            "DIMENSION_SHADER_ARRAY[0:1]",
            "DIMENSION_SHADER_ENGINE[0:1]",
        ):
            assert marker in list_output

        agent = data["agent"]["stdout"]
        for name, value in (
            ("name", "gfx1151"),
            ("node_id", "1"),
            ("cu_count", "40"),
            ("simd_count", "80"),
            ("wave_front_size", "32"),
            ("gfx_target_version", "110501"),
        ):
            require_agent_field(agent, name, value)

        identity, actual_counters = parse_counter_info(data["catalog"]["stdout"])
        expected_catalog = PMC_CONTRACT["counter_catalog"]
        assert identity == {"logical_gpu_index": 0, "architecture": "gfx1151"}
        assert len(actual_counters) == expected_catalog["count"] == 442
        assert [counter["name"] for counter in actual_counters] == sorted(
            counter["name"] for counter in actual_counters
        )
        assert Counter(counter["kind"] for counter in actual_counters) == Counter(
            expected_catalog["kind_counts"]
        )
        expected_counters = {
            counter["name"]: counter for counter in expected_catalog["counters"]
        }
        actual_counters_by_name = {
            counter["name"]: counter for counter in actual_counters
        }
        assert actual_counters_by_name.keys() == expected_counters.keys()
        for name, expected in expected_counters.items():
            assert actual_counters_by_name[name] == expected, name

        observations = {
            item["name"]: item for item in PMC_CONTRACT["profile_observations"]
        }
        assert (
            observations["retained-three-counter-group"]["diagnostic"]
            in data["pmc_check"]["stdout"]
        )
        assert (
            observations["representative-multi-block-group"]["diagnostic"]
            in data["pmc_check_representative"]["stdout"]
        )
        assert (
            "Following input counters can be collected together on GPU:0\tGDSInsts"
            in data["pmc_check_derived"]["stdout"]
        )
        assert (
            "Following input counters can be collected together on GPU:0\t"
            "GRBM_GL2C_BUSY\tGCEA_RDRAM_SIZE_REQ\tGCEA_WDRAM_SIZE_REQ"
            in data["pmc_check_catalog_boundaries"]["stdout"]
        )
        assert (
            observations["grbm-capacity-exceeded"]["diagnostic"]
            in data["pmc_check_rejected"]["stdout"]
        )
        assert (
            observations["unknown-counter"]["diagnostic"]
            in data["pmc_check_unknown"]["stdout"]
        )
    elif case == "baseline":
        assert data["returncode"] == 0
        require_workload(data["stdout"])
        assert "runtime-run\\amdhip64_7.dll" in data["stdout"]
    elif case == "kernel-trace":
        assert data["returncode"] == 0
        require_workload(data["stdout"])
        require_original_target(data)
        assert "Windows kernel trace: records=8" in data["stdout"]
        rows = [row for row in data["rows"] if "vector_add" in row["Kernel_Name"]]
        assert len(rows) == 8
        assert {int(row["Dispatch_Id"]) for row in rows} == set(range(1, 9))
        assert len({int(row["Queue_Id"]) for row in rows}) == 1
        for row in rows:
            assert row["Kind"] == "KERNEL_DISPATCH"
            assert row["Agent_Id"] == "Agent 1"
            assert int(row["Queue_Id"]) >= 0
            assert int(row["Stream_Id"]) > 0
            assert int(row["Thread_Id"]) > 0
            assert int(row["Kernel_Id"]) > 0
            assert int(row["Correlation_Id"]) > 0
            assert int(row["End_Timestamp"]) > int(row["Start_Timestamp"])
            assert int(row["Workgroup_Size_X"]) == 256
            assert int(row["Workgroup_Size_Y"]) == 1
            assert int(row["Workgroup_Size_Z"]) == 1
            assert int(row["Grid_Size_X"]) == 1_048_576
            assert int(row["Grid_Size_Y"]) == 1
            assert int(row["Grid_Size_Z"]) == 1
    elif case == "dispatch-analysis-contract":
        assert data["contract_version"] == 3
        standalone = data["standalone"]
        composed = data["composed"]
        enqueue_sequence = data["enqueue_sequence"]
        resource_metadata = data["resource_metadata"]
        resource_fields = (
            "LDS_Block_Size",
            "Scratch_Size",
            "VGPR_Count",
            "Accum_VGPR_Count",
            "SGPR_Count",
        )

        def base_kernel_names(rows):
            return [
                next(
                    name for name in set(enqueue_sequence) if name in row["Kernel_Name"]
                )
                for row in rows
            ]

        def validate_resource_rows(rows):
            for row, name in zip(rows, base_kernel_names(rows)):
                assert {
                    field: int(row[field]) for field in resource_fields
                } == resource_metadata[name]

        assert standalone["returncode"] == 0
        assert composed["returncode"] == 0
        for run in (standalone, composed):
            assert "dispatch_analysis=passed" in run["stdout"]
            assert "architecture=gfx1151" in run["stdout"]
            assert "dispatches=6" in run["stdout"]
            assert "streams=2" in run["stdout"]

        # Standalone CLR activity uses its explicit enqueue ordinal for the same
        # formatted-name, range, and authoritative resource-metadata semantics.
        standalone_rows = standalone["trace_rows"]
        assert standalone["trace_exists"]
        assert not standalone["stats_exists"]
        assert len(standalone_rows) == 1
        assert int(standalone_rows[0]["Dispatch_Id"]) == 3
        assert "dispatch_vector" in standalone_rows[0]["Kernel_Name"]
        validate_resource_rows(standalone_rows)

        expected_case_names = {
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
        selection_cases = data["selection_cases"]
        assert {entry["name"] for entry in selection_cases} == expected_case_names

        def recompute_stats(trace_rows):
            samples = {}
            for row in trace_rows:
                duration = int(row["End_Timestamp"]) - int(row["Start_Timestamp"])
                assert duration > 0
                samples.setdefault(row["Kernel_Name"], []).append(duration)
            total = sum(sum(values) for values in samples.values())
            output = {}
            for name, values in samples.items():
                count = len(values)
                duration_sum = sum(values)
                square_sum = sum(value * value for value in values)
                variance = 0.0
                if count > 1:
                    variance = (square_sum - (duration_sum * duration_sum) / count) / (
                        count - 1
                    )
                output[name] = {
                    "count": count,
                    "sum": duration_sum,
                    "sqr": square_sum,
                    "min": min(values),
                    "max": max(values),
                    "mean": duration_sum / count,
                    "variance": variance,
                    "stddev": math.sqrt(abs(variance)),
                    "percentage": (duration_sum / total) * 100.0,
                }
            return output

        def validate_stats_csv(trace_rows, stats_rows):
            expected = recompute_stats(trace_rows)
            assert {row["Name"] for row in stats_rows} == set(expected)
            assert [row["Name"] for row in stats_rows] == sorted(
                expected, key=lambda name: (-expected[name]["sum"], name)
            )
            for row in stats_rows:
                values = expected[row["Name"]]
                assert int(row["Calls"]) == values["count"]
                assert int(row["TotalDurationNs"]) == values["sum"]
                assert int(row["MinNs"]) == values["min"]
                assert int(row["MaxNs"]) == values["max"]
                assert math.isclose(
                    float(row["AverageNs"]), values["mean"], rel_tol=1.0e-6
                )
                assert math.isclose(
                    float(row["StdDev"]),
                    values["stddev"],
                    rel_tol=1.0e-6,
                    abs_tol=1.0e-6,
                )
                assert math.isclose(
                    float(row["Percentage"]),
                    values["percentage"],
                    rel_tol=1.0e-3,
                    abs_tol=1.0e-2,
                )
            assert math.isclose(
                sum(float(row["Percentage"]) for row in stats_rows),
                100.0,
                abs_tol=2.0e-2,
            )

        def validate_json_summary(trace_rows, summary):
            expected = recompute_stats(trace_rows)
            assert len(summary) == 1
            assert summary[0]["domain"] == "KERNEL_DISPATCH"
            stats = summary[0]["stats"]
            assert stats["cereal_class_version"] == 0
            total_count = sum(value["count"] for value in expected.values())
            total_sum = sum(value["sum"] for value in expected.values())
            total_sqr = sum(value["sqr"] for value in expected.values())
            total_variance = (total_sqr - (total_sum * total_sum) / total_count) / (
                total_count - 1
            )
            assert stats["count"] == total_count
            assert stats["sum"] == total_sum
            assert stats["sqr"] == total_sqr
            assert stats["min"] == min(value["min"] for value in expected.values())
            assert stats["max"] == max(value["max"] for value in expected.values())
            assert math.isclose(stats["mean"], total_sum / total_count, rel_tol=1.0e-12)
            assert math.isclose(stats["variance"], total_variance, rel_tol=1.0e-12)
            assert math.isclose(
                stats["stddev"], math.sqrt(abs(total_variance)), rel_tol=1.0e-12
            )
            operation_entries = stats["operations"]
            assert [entry["key"] for entry in operation_entries] == sorted(expected)
            assert operation_entries[0]["value"]["cereal_class_version"] == 0
            assert all(
                "cereal_class_version" not in entry["value"]
                for entry in operation_entries[1:]
            )
            for entry in operation_entries:
                name = entry["key"]
                operation = entry["value"]
                values = expected[name]
                for field in ("count", "sum", "sqr", "min", "max"):
                    assert operation[field] == values[field]
                for field in ("mean", "variance", "stddev"):
                    assert math.isclose(
                        operation[field],
                        values[field],
                        rel_tol=1.0e-12,
                        abs_tol=1.0e-12,
                    )

        for entry in selection_cases:
            expected_ids = entry["selected_enqueue_ordinals"]
            expected_names = [enqueue_sequence[index - 1] for index in expected_ids]
            standalone_case = entry["standalone"]
            composed_case = entry["composed"]
            assert standalone_case["returncode"] == 0
            assert composed_case["returncode"] == 0
            for run in (standalone_case, composed_case):
                assert "dispatch_analysis=passed" in run["stdout"]
                assert "dispatches=6" in run["stdout"]

            standalone_case_rows = standalone_case["trace_rows"]
            assert standalone_case["trace_exists"]
            assert [
                int(row["Dispatch_Id"]) for row in standalone_case_rows
            ] == expected_ids
            assert base_kernel_names(standalone_case_rows) == expected_names
            validate_resource_rows(standalone_case_rows)

            composed_case_rows = composed_case["trace_rows"]
            composed_counter_rows = composed_case["counter_rows"]
            if expected_ids:
                assert composed_case["trace_exists"]
                assert composed_case["json_exists"]
            else:
                assert not composed_case["trace_exists"]
                assert not composed_case["json_exists"]
            assert [
                int(row["Dispatch_Id"]) for row in composed_case_rows
            ] == expected_ids
            assert base_kernel_names(composed_case_rows) == expected_names
            validate_resource_rows(composed_case_rows)
            validate_resource_rows(composed_counter_rows)
            assert len(composed_counter_rows) == 3 * len(expected_ids)
            assert {int(row["Dispatch_Id"]) for row in composed_counter_rows} == set(
                expected_ids
            )
            assert [
                record["dispatch_info"]["dispatch_id"]
                for record in composed_case["json_kernel_records"]
            ] == expected_ids

            expect_stats = entry["stats_requested"] and bool(expected_ids)
            assert standalone_case["stats_exists"] == expect_stats
            assert composed_case["stats_exists"] == expect_stats
            if expect_stats:
                validate_stats_csv(standalone_case_rows, standalone_case["stats_rows"])
                validate_stats_csv(composed_case_rows, composed_case["stats_rows"])
                validate_json_summary(composed_case_rows, composed_case["json_summary"])
            else:
                assert standalone_case["stats_rows"] == []
                assert composed_case["stats_rows"] == []
                assert composed_case["json_summary"] == []

            if entry["name_mode"] == "mangled":
                assert all(
                    row["Kernel_Name"].startswith("_Z")
                    for row in standalone_case_rows + composed_case_rows
                )
            elif entry["name_mode"] == "truncated":
                assert {row["Kernel_Name"] for row in standalone_case_rows} == {
                    "dispatch_vector"
                }
                assert {row["Kernel_Name"] for row in composed_case_rows} == {
                    "dispatch_vector"
                }

        counter_rows = composed["counter_rows"]
        trace_rows = composed["trace_rows"]
        assert composed["json_exists"]
        assert composed["trace_exists"]
        assert not composed["stats_exists"]
        assert len(counter_rows) == 18
        assert len(trace_rows) == 6
        assert {row["Counter_Name"] for row in counter_rows} == {
            "L2CacheHit",
            "VALUInsts",
            "LDSBankConflict",
        }
        assert {int(row["Dispatch_Id"]) for row in counter_rows} == set(range(1, 7))
        assert all(int(row["Start_Timestamp"]) > 0 for row in counter_rows)
        assert all(
            int(row["End_Timestamp"]) > int(row["Start_Timestamp"])
            for row in counter_rows
        )
        validate_resource_rows(trace_rows)
        validate_resource_rows(counter_rows)

        counter_by_dispatch = {}
        for row in counter_rows:
            counter_by_dispatch.setdefault(int(row["Dispatch_Id"]), row)
        trace_by_dispatch = {int(row["Dispatch_Id"]): row for row in trace_rows}
        assert set(trace_by_dispatch) == set(counter_by_dispatch) == set(range(1, 7))
        for dispatch_id, trace in trace_by_dispatch.items():
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
                assert trace[field] == counter[field]

        json_records = composed["json_kernel_records"]
        assert len(json_records) == 6
        assert [
            record["dispatch_info"]["dispatch_id"] for record in json_records
        ] == list(range(1, 7))
        for record in json_records:
            trace = trace_by_dispatch[record["dispatch_info"]["dispatch_id"]]
            assert record["thread_id"] == int(trace["Thread_Id"])
            assert record["correlation_id"]["internal"] == int(trace["Correlation_Id"])
            assert record["start_timestamp"] == int(trace["Start_Timestamp"])
            assert record["end_timestamp"] == int(trace["End_Timestamp"])
            assert record["dispatch_info"]["queue_id"]["handle"] == int(
                trace["Queue_Id"]
            )
            assert record["dispatch_info"]["kernel_id"] == int(trace["Kernel_Id"])
            assert record["dispatch_info"]["group_segment_size"] == int(
                trace["LDS_Block_Size"]
            )
            assert record["dispatch_info"]["private_segment_size"] == int(
                trace["Scratch_Size"]
            )

        json_symbols = composed["json_kernel_symbols"]
        assert len(json_symbols) == len(resource_metadata)
        for symbol in json_symbols:
            name = next(
                name
                for name in resource_metadata
                if name in symbol["formatted_kernel_name"]
            )
            expected = resource_metadata[name]
            assert symbol["kernel_object"] > 0
            assert symbol["kernel_address"]["handle"] > 0
            assert symbol["group_segment_size"] == expected["LDS_Block_Size"]
            assert symbol["private_segment_size"] == expected["Scratch_Size"]
            assert symbol["arch_vgpr_count"] == expected["VGPR_Count"]
            assert symbol["accum_vgpr_count"] == expected["Accum_VGPR_Count"]
            assert symbol["sgpr_count"] == expected["SGPR_Count"]

        rocpd = data["rocpd"]
        assert rocpd["returncode"] == 0
        assert rocpd["exists"]
        assert not rocpd["internal_json_exists"]
        assert rocpd["integrity"] == "ok"
        assert rocpd["foreign_key_errors"] == []
        assert "Windows ROCpd: dispatches=6 counters=18 kernel_symbols=3" in rocpd[
            "stdout"
        ]
        assert rocpd["metadata"]["schema_version"] == "3.0.3"
        assert rocpd["metadata"]["producer"] == "rocprofv3-windows-post-target"
        assert rocpd["metadata"]["source_format"] == "rocprofiler-sdk-tool-json"
        assert rocpd["metadata"]["dispatch_count"] == "6"
        assert rocpd["metadata"]["counter_record_count"] == "18"
        assert {
            "rocpd_kernel_dispatch",
            "rocpd_info_kernel_symbol",
            "rocpd_info_pmc",
            "rocpd_pmc_event",
            "kernels",
            "kernel_symbols",
            "pmc_events",
            "top_kernels",
            "top",
        }.issubset(rocpd["schema_objects"])

        database_kernels = rocpd["kernels"]
        assert [row["dispatch_id"] for row in database_kernels] == list(range(1, 7))
        assert [
            next(name for name in set(enqueue_sequence) if name in row["name"])
            for row in database_kernels
        ] == enqueue_sequence
        for row, name in zip(database_kernels, enqueue_sequence):
            assert row["kernel_id"] > 0
            assert row["tid"] > 0
            assert row["agent_abs_index"] == 1
            assert row["queue_id"] > 0
            assert row["stream_id"] >= 0
            assert row["start"] > 0
            assert row["end"] > row["start"]
            assert row["duration"] == row["end"] - row["start"]
            assert (row["grid_x"], row["grid_y"], row["grid_z"]) == (
                1_048_576,
                1,
                1,
            )
            assert (
                row["workgroup_x"],
                row["workgroup_y"],
                row["workgroup_z"],
            ) == (256, 1, 1)
            expected = resource_metadata[name]
            assert {
                "LDS_Block_Size": row["lds_size"],
                "Scratch_Size": row["scratch_size"],
                "VGPR_Count": row["vgpr_count"],
                "Accum_VGPR_Count": row["accum_vgpr_count"],
                "SGPR_Count": row["sgpr_count"],
            } == expected

        database_pmc = rocpd["pmc_events"]
        assert len(database_pmc) == 18
        assert {row["dispatch_id"] for row in database_pmc} == set(range(1, 7))
        assert {row["counter_name"] for row in database_pmc} == {
            "L2CacheHit",
            "VALUInsts",
            "LDSBankConflict",
        }
        database_kernel_by_id = {
            row["dispatch_id"]: row for row in database_kernels
        }
        for row in database_pmc:
            kernel = database_kernel_by_id[row["dispatch_id"]]
            assert row["name"] == kernel["name"]
            assert row["start"] == kernel["start"]
            assert row["end"] == kernel["end"]
            assert row["duration"] == kernel["duration"]
            assert math.isfinite(row["counter_value"])

        database_symbols = rocpd["kernel_symbols"]
        assert len(database_symbols) == len(resource_metadata)
        for symbol in database_symbols:
            name = next(
                name
                for name in resource_metadata
                if name in symbol["formatted_kernel_name"]
            )
            expected = resource_metadata[name]
            assert {
                "LDS_Block_Size": symbol["group_segment_size"],
                "Scratch_Size": symbol["private_segment_size"],
                "VGPR_Count": symbol["arch_vgpr_count"],
                "Accum_VGPR_Count": symbol["accum_vgpr_count"],
                "SGPR_Count": symbol["sgpr_count"],
            } == expected

        top_kernels = rocpd["top_kernels"]
        assert len(top_kernels) == len(resource_metadata)
        expected_calls = {
            "dispatch_vector": 3,
            "dispatch_lds_conflict": 2,
            "dispatch_resource": 1,
        }
        assert {
            next(name for name in expected_calls if name in row["name"]): row[
                "total_calls"
            ]
            for row in top_kernels
        } == expected_calls
        durations_by_name = {}
        for kernel in database_kernels:
            durations_by_name.setdefault(kernel["name"], []).append(kernel["duration"])
        total_database_duration = sum(
            sum(durations) for durations in durations_by_name.values()
        )
        for row in top_kernels:
            durations = durations_by_name[row["name"]]
            duration_sum = sum(durations)
            assert math.isclose(
                row["total_duration"], duration_sum / 1000.0, rel_tol=1.0e-12
            )
            assert math.isclose(
                row["average"],
                (duration_sum // len(durations)) / 1000.0,
                rel_tol=1.0e-12,
            )
            assert math.isclose(
                row["percentage"],
                duration_sum * 100.0 / total_database_duration,
                rel_tol=1.0e-12,
            )
        assert math.isclose(
            sum(row["percentage"] for row in top_kernels), 100.0, abs_tol=1.0e-9
        )
    elif case == "hip-trace":
        assert data["returncode"] == 0
        require_workload(data["stdout"])
        require_original_target(data)
        assert "Windows trace: hip_records=18" in data["stdout"]
        assert data["graph_output_exists"]
        assert data["graph_rows"] == []
        rows = data["rows"]
        assert rows
        functions = [row["Function"] for row in rows]
        assert functions.count("hipMalloc") == 3
        assert functions.count("hipLaunchKernel") == 8
        assert functions.count("hipFree") == 3
        assert functions.count("hipMemcpy") == 3
        assert functions.count("hipDeviceSynchronize") == 1
        correlation_ids = [int(row["Correlation_Id"]) for row in rows]
        assert len(correlation_ids) == len(set(correlation_ids))
        assert all(row["Domain"] == "HIP_RUNTIME_API" for row in rows)
        assert all(int(row["Process_Id"]) > 0 for row in rows)
        assert all(int(row["Thread_Id"]) > 0 for row in rows)
        assert all(int(row["Status"]) == 0 for row in rows)
        assert all(
            int(row["End_Timestamp"]) >= int(row["Start_Timestamp"]) for row in rows
        )
    elif case == "hip-graph":
        assert data["returncode"] == 0
        require_original_target(data)
        for marker in (
            "architecture=gfx1151",
            "dispatches=2",
            "kernel=vector_add",
            "execution=graph",
            "resource=loaded",
            "validation=passed",
            "Windows trace: hip_records=17",
            "graph_records=2",
        ):
            assert marker in data["stdout"]
        assert not data["api_output_exists"]
        assert data["rows"] == []
        assert data["graph_output_exists"]
        graph_rows = data["graph_rows"]
        assert len(graph_rows) == 2
        assert {row["Kind"] for row in graph_rows} == {"HIP_GRAPH_LAUNCH"}
        assert {row["Kernel_Dispatch_Count"] for row in graph_rows} == {"1"}
        assert len({row["Graph_Exec_Id"] for row in graph_rows}) == 1
        assert graph_rows[0]["Graph_Exec_Id"] not in ("", "0x0")
        assert {row["Status"] for row in graph_rows} == {"0"}
        correlations = [int(row["Correlation_Id"]) for row in graph_rows]
        assert len(correlations) == len(set(correlations))
        assert all(value > 0 for value in correlations)
    elif case == "hip-marker":
        assert data["returncode"] == 0
        require_original_target(data)
        for marker in (
            "architecture=gfx1151",
            "dispatches=2",
            "execution=direct",
            "resource=loaded",
            "markers=enabled",
            "validation=passed",
            "hip_records=12",
            "marker_api_records=6",
            "marker_records=4",
        ):
            assert marker in data["stdout"]
        assert data["graph_output_exists"]
        assert data["graph_rows"] == []
        hip_rows = data["rows"]
        marker_api_rows = data["marker_api_rows"]
        marker_rows = data["marker_rows"]
        assert len(hip_rows) == 12
        assert len(marker_api_rows) == 6
        assert len(marker_rows) == 4
        assert [row["Message"] for row in marker_rows] == [
            "hip workload begin",
            "hip dispatches",
            "hip workload",
            "hip workload end",
        ]
        all_api_rows = hip_rows + marker_api_rows
        correlations = [int(row["Correlation_Id"]) for row in all_api_rows]
        assert len(correlations) == len(set(correlations))
        assert all(value > 0 for value in correlations)
        assert {row["Correlation_Id"] for row in marker_rows} < {
            row["Correlation_Id"] for row in marker_api_rows
        }
        assert min(int(row["Correlation_Id"]) for row in marker_api_rows) < min(
            int(row["Correlation_Id"]) for row in hip_rows
        )
        assert {row["Status"] for row in hip_rows} == {"0"}
        assert {row["Status"] for row in marker_rows} == {"0"}
    elif case == "roctx-trace":
        assert data["returncode"] == 0
        for marker in (
            "roctx_workload=passed",
            "mark=1",
            "thread_ranges=2",
            "process_ranges=1",
            "marker_api_records=8",
            "marker_records=4",
        ):
            assert marker in data["stdout"]
        api_rows = data["api_rows"]
        marker_rows = data["marker_rows"]
        assert len(api_rows) == 8
        assert len(marker_rows) == 4
        assert {row["Domain"] for row in api_rows} == {"MARKER_CORE_API"}
        functions = [row["Function"] for row in api_rows]
        assert functions.count("roctxMarkA") == 1
        assert functions.count("roctxRangePushA") == 2
        assert functions.count("roctxRangePop") == 3
        assert functions.count("roctxRangeStartA") == 1
        assert functions.count("roctxRangeStop") == 1
        assert [row["Status"] for row in api_rows].count("-1") == 1
        correlations = [int(row["Correlation_Id"]) for row in api_rows]
        assert all(value > 0 for value in correlations)
        assert len(correlations) == len(set(correlations))
        assert {row["Kind"] for row in marker_rows} == {
            "mark",
            "thread_range",
            "process_range",
        }
        assert [row["Message"] for row in marker_rows] == [
            "standalone mark",
            "inner range",
            "outer range",
            "process range",
        ]
        assert {row["Status"] for row in marker_rows} == {"0"}
        api_correlations = {row["Correlation_Id"] for row in api_rows}
        assert {row["Correlation_Id"] for row in marker_rows} < api_correlations
        for row in marker_rows:
            start = int(row["Start_Timestamp"])
            end = int(row["End_Timestamp"])
            assert start > 0
            assert end >= start
    elif case == "no-overwrite":
        assert data["returncode"] == 1
        assert "output already exists" in data["stdout"]
        assert data["retained_output"] == "retained"
        assert not data["target_launched"]
    elif case == "hsa-barrier":
        assert data["returncode"] == 0
        assert data["hsa_tools_lib"] is None
        assert data["sdk_path"].endswith("\\bin\\rocprofiler-sdk.dll")
        assert data["registration_environment"] == {
            "ROCPROFILER_REGISTER_ENABLED": "1",
            "ROCPROFILER_REGISTER_FORCE_LOAD": "1",
            "ROCPROFILER_REGISTER_LIBRARY": data["sdk_path"],
            "ROCPROFILER_REGISTER_SECURE": "1",
        }
        stdout = data["stdout"]
        for marker in (
            "hsa_init=0x0",
            "hsa_iterate_agents=0x0",
            "hsa_queue_create=0x0",
            "hsa_signal_create=0x0",
            "completion=0",
            "hsa_signal_destroy=0x0",
            "hsa_shut_down=0x0",
        ):
            assert marker in stdout
        assert re.search(r"gpu_agents=[1-9]\d*", stdout)
        tool_log = data["tool_log"]
        for marker in (
            "event=api_table status=accepted name=hsa",
            "event=onload",
            "status=accepted",
            "event=queue_create status=intercepted",
            "create_status=0 register_status=0",
        ):
            assert marker in tool_log
        workload_packet = re.search(
            r"barrier_packet queue_id=(\d+) packet_id=(\d+) completion=0",
            stdout,
        )
        tool_packet = re.search(
            r"event=packet queue_id=(\d+) packet_id=(\d+) packet_type=3",
            tool_log,
        )
        assert workload_packet and tool_packet
        assert workload_packet.groups() == tool_packet.groups()
        assert set(re.findall(r"packet_type=(\d+)", tool_log)) == {"3"}
    else:
        raise AssertionError(case)
