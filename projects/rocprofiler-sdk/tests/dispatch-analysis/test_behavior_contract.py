from __future__ import annotations

import json
from pathlib import Path


CONTRACT_PATH = Path(__file__).with_name("behavior_cases.json")
LINUX_ORACLE_PATH = Path(__file__).with_name("gfx1151_linux_oracle.json")


def load_contract():
    return json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))


def test_dispatch_analysis_contract_is_complete():
    contract = load_contract()
    assert contract["version"] == 1
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


def test_linux_oracle_freezes_schema_shape_and_current_filter_scope():
    oracle = json.loads(LINUX_ORACLE_PATH.read_text(encoding="utf-8"))
    assert oracle["version"] == 1
    assert oracle["architecture"] == "gfx1151"
    assert oracle["workload_dispatches"] == 6
    assert oracle["csv"]["kernel_trace"]["rows"] == 6
    assert oracle["csv"]["counter_collection"]["rows"] == 18
    assert oracle["csv"]["kernel_stats"]["rows"] == 3
    assert oracle["invariants"]["dispatch_ids"] == list(range(1, 7))
    assert oracle["invariants"]["counter_rows_per_dispatch"] == 3
    selection = oracle["selection_oracle"]
    assert selection["selected_counter_dispatch_ids"] == [3]
    assert selection["kernel_trace_dispatch_ids"] == list(range(1, 7))
