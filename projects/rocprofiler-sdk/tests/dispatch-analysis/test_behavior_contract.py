from __future__ import annotations

import json
from pathlib import Path


CONTRACT_PATH = Path(__file__).with_name("behavior_cases.json")
LINUX_ORACLE_PATH = Path(__file__).with_name("gfx1151_linux_oracle.json")


def load_contract():
    return json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))


def test_dispatch_analysis_contract_is_complete():
    contract = load_contract()
    assert contract["version"] == 3
    workload = contract["workload"]
    sequence = workload["enqueue_sequence"]
    assert workload["dispatch_count"] == len(sequence) == 6
    assert workload["streams"] == [0, 1]
    assert workload["join_key"] == ["Process_Id", "Dispatch_Id"]
    assert set(workload["mandatory_identity"]) == {
        "Queue_Id",
        "Kernel_Id",
        "Correlation_Id",
        "Dispatch_Id",
    }
    assert sequence == [
        "dispatch_vector",
        "dispatch_lds_conflict",
        "dispatch_vector",
        "dispatch_lds_conflict",
        "dispatch_vector",
        "dispatch_resource",
    ]
    windows_resources = workload["gfx1151_windows_resource_metadata"]
    linux_resources = workload["gfx1151_linux_resource_metadata"]
    assert windows_resources == {
        "dispatch_vector": {
            "LDS_Block_Size": 0,
            "Scratch_Size": 0,
            "VGPR_Count": 8,
            "Accum_VGPR_Count": 0,
            "SGPR_Count": 128,
        },
        "dispatch_lds_conflict": {
            "LDS_Block_Size": 8192,
            "Scratch_Size": 0,
            "VGPR_Count": 8,
            "Accum_VGPR_Count": 0,
            "SGPR_Count": 128,
        },
        "dispatch_resource": {
            "LDS_Block_Size": 4096,
            "Scratch_Size": 132,
            "VGPR_Count": 24,
            "Accum_VGPR_Count": 0,
            "SGPR_Count": 128,
        },
    }
    assert set(linux_resources) == set(windows_resources)
    for name in windows_resources:
        for field in (
            "LDS_Block_Size",
            "VGPR_Count",
            "Accum_VGPR_Count",
            "SGPR_Count",
        ):
            assert linux_resources[name][field] == windows_resources[name][field]
    assert linux_resources["dispatch_vector"]["Scratch_Size"] == 0
    assert linux_resources["dispatch_lds_conflict"]["Scratch_Size"] == 0
    assert linux_resources["dispatch_resource"]["Scratch_Size"] == 144
    assert contract["counter_group"] == [
        "L2CacheHit",
        "VALUInsts",
        "LDSBankConflict",
    ]

    cases = contract["cases"]
    names = [case["name"] for case in cases]
    assert len(names) == len(set(names))
    assert set(names) == {
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
        "no-dispatch",
        "target-failure",
        "unknown-counter",
        "output-conflict",
    }
    for case in cases:
        ordinals = case["selected_enqueue_ordinals"]
        assert ordinals == sorted(set(ordinals))
        assert all(1 <= value <= workload["dispatch_count"] for value in ordinals)
        assert all(isinstance(value, str) for value in case["profiler_args"])
        assert all(isinstance(value, str) for value in case["target_args"])


def test_iteration_cases_are_per_formatted_kernel_enqueue_order():
    cases = {case["name"]: case for case in load_contract()["cases"]}
    assert cases["vector-iteration-two"]["selected_enqueue_ordinals"] == [3]
    assert cases["vector-iteration-range"]["selected_enqueue_ordinals"] == [1, 3]
    assert cases["reversed-completion"]["selected_enqueue_ordinals"] == [3]
    assert cases["mangled-vector"]["name_mode"] == "mangled"
    assert cases["truncated-vector"]["name_mode"] == "truncated"


def test_linux_oracle_freezes_schema_shape_and_shared_dispatch_selection():
    oracle = json.loads(LINUX_ORACLE_PATH.read_text(encoding="utf-8"))
    assert oracle["version"] == 2
    assert oracle["architecture"] == "gfx1151"
    assert oracle["workload_dispatches"] == 6
    assert oracle["csv"]["kernel_trace"]["rows"] == 6
    assert oracle["csv"]["counter_collection"]["rows"] == 18
    assert oracle["csv"]["kernel_stats"]["rows"] == 3
    assert oracle["invariants"]["dispatch_ids"] == list(range(1, 7))
    assert oracle["invariants"]["counter_rows_per_dispatch"] == 3
    assert oracle["invariants"]["nonzero_scratch_kernel"] == "dispatch_resource"
    assert oracle["invariants"]["resource_metadata"] == load_contract()["workload"][
        "gfx1151_linux_resource_metadata"
    ]
    selection = oracle["selection_oracle"]
    expected_ids = selection["selected_dispatch_ids"]
    assert expected_ids == [3]
    for key in (
        "counter_dispatch_ids",
        "kernel_trace_dispatch_ids",
        "json_counter_dispatch_ids",
        "json_kernel_dispatch_ids",
        "rocpd_dispatch_ids",
    ):
        assert selection[key] == expected_ids
    assert selection["statistics_calls"] == len(expected_ids)
