#!/usr/bin/env python3

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
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

from __future__ import annotations

import copy
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

TEST_DIRECTORY = Path(__file__).resolve().parent
CONTRACT_PATH = Path(
    os.environ.get("ROCPROFILER_PMC_GFX1151_CONTRACT", TEST_DIRECTORY / "gfx1151_linux_contract.json")
).resolve()
TOOL_PATH = TEST_DIRECTORY / "pmc_contract.py"

sys.path.insert(0, str(TEST_DIRECTORY))
from pmc_contract import compare_contracts, parse_counter_info  # noqa: E402


@pytest.fixture(scope="session")
def contract():
    return json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))


def test_oracle_provenance_and_catalog(contract):
    assert contract["schema_version"] == 1
    assert contract["oracle"]["repository_revision"] == "0a26f210e08d336a57dc09d027ae1dda5f48c37d"
    assert contract["oracle"]["repository_branch"] == "pc_sampling_gfx1151"
    assert contract["counter_catalog"]["architecture"] == "gfx1151"
    assert contract["counter_catalog"]["count"] == 442
    assert contract["counter_catalog"]["kind_counts"] == {
        "constant": 59,
        "derived": 155,
        "raw": 228,
    }
    counters = {item["name"]: item for item in contract["counter_catalog"]["counters"]}
    assert len(counters) == 442
    assert counters["GRBM_COUNT"]["block"] == "GRBM"
    assert counters["GRBM_GUI_ACTIVE"]["block"] == "GRBM"
    assert counters["SQ_WAVES"]["block"] == "SQ"
    assert counters["SQ_WAVES"]["dimensions"][-1] == {
        "name": "DIMENSION_SHADER_ENGINE",
        "minimum": 0,
        "maximum": 1,
    }
    assert counters["SQ_WAVES_sum"]["kind"] == "derived"
    assert counters["SQ_WAVES_sum"]["expression"] == "reduce(SQ_WAVES,sum)"


def test_cli_profiles_and_output_contract(contract):
    assert contract["cli"]["output_formats"] == ["csv", "json", "pftrace", "otf2", "rocpd"]
    assert contract["cli"]["multiple_pass_syntax"] == "repeated --pmc"
    observations = {item["name"]: item for item in contract["profile_observations"]}
    assert observations["retained-three-counter-group"]["collectable_together"] is True
    assert observations["representative-multi-block-group"]["collectable_together"] is True
    assert observations["grbm-capacity-exceeded"]["collectable_together"] is False
    assert observations["unknown-counter"]["exit_status"] == 1

    behavior = contract["behavior"]
    assert behavior["no_dispatch"]["exit_status"] == 0
    assert behavior["no_dispatch"]["files"] == []
    assert behavior["target_failure"]["exit_status"] == 7
    assert behavior["target_failure"]["files"] == []
    assert behavior["unsupported"]["exit_status"] == 0
    assert behavior["unsupported"]["files"] == []
    assert set(behavior["composed"]["files"]) == {
        "oracle_agent_info.csv",
        "oracle_counter_collection.csv",
        "oracle_kernel_trace.csv",
        "oracle_results.json",
    }


def test_dispatch_filter_and_multipass_semantics(contract):
    behavior = contract["behavior"]
    filtered = behavior["filtered"]["counter_outputs"]["oracle_counter_collection.csv"]
    assert filtered["rows"] == 2
    assert filtered["counter_names"] == ["SQ_WAVES"]
    assert filtered["dispatch_ids"] == [4, 5]
    assert filtered["vector_add_sq_waves_values"] == [32768.0]

    pass_1 = behavior["multipass"]["counter_outputs"][
        "pass_1/oracle_counter_collection.csv"
    ]
    pass_2 = behavior["multipass"]["counter_outputs"][
        "pass_2/oracle_counter_collection.csv"
    ]
    assert pass_1["counter_names"] == ["SQ_WAVES"]
    assert pass_2["counter_names"] == ["GRBM_COUNT"]
    assert pass_1["dispatch_ids"] == pass_2["dispatch_ids"] == list(range(1, 12))

    composed = behavior["composed"]["counter_outputs"]["oracle_counter_collection.csv"]
    assert composed["rows"] == 33
    assert composed["counter_names"] == ["GRBM_COUNT", "GRBM_GUI_ACTIVE", "SQ_WAVES"]
    assert composed["dispatch_ids"] == list(range(1, 12))
    assert composed["vector_add_sq_waves_values"] == [32768.0]


def test_counter_info_parser_classifies_metadata():
    identity, counters = parse_counter_info(
        """GPU:0
Name:gfx1151
Counter_Name        :\tRAW
Description         :\traw counter
Block               :\tSQ
SPM                 :\tNot Supported
Dimensions          :\tDIMENSION_INSTANCE[0:0] DIMENSION_SHADER_ENGINE[0:1]

Counter_Name        :\tDERIVED
Description         :\tderived counter
Expression          :\treduce(RAW,sum)
Dimensions          :\tDIMENSION_INSTANCE[0:0]

Counter_Name        :\tconstant
Description         :\tConstant value
"""
    )
    assert identity == {"logical_gpu_index": 0, "architecture": "gfx1151"}
    records = {item["name"]: item for item in counters}
    assert records["RAW"]["kind"] == "raw"
    assert records["DERIVED"]["kind"] == "derived"
    assert records["constant"]["kind"] == "constant"
    assert records["RAW"]["dimensions"][1]["maximum"] == 1


def test_comparator_ignores_provenance_but_detects_semantic_change(contract):
    actual = copy.deepcopy(contract)
    actual["oracle"]["rocprofv3"]["version"] = "future"
    actual["behavior"]["composed"]["counter_outputs"]["oracle_counter_collection.csv"][
        "vector_add_sq_waves_values"
    ] = [999.0]
    assert compare_contracts(contract, actual) == []

    actual["counter_catalog"]["counters"][0]["description"] = "changed"
    differences = compare_contracts(contract, actual)
    assert len(differences) == 1
    assert differences[0].startswith("counter_catalog.")


def test_compare_command(contract, tmp_path):
    actual = tmp_path / "actual.json"
    actual.write_text(json.dumps(contract), encoding="utf-8")
    completed = subprocess.run(
        [
            sys.executable,
            str(TOOL_PATH),
            "compare",
            "--expected",
            str(CONTRACT_PATH),
            "--actual",
            str(actual),
        ],
        text=True,
        capture_output=True,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr
    assert "pmc_contract_compare=passed architecture=gfx1151 counters=442" in completed.stdout

    changed = copy.deepcopy(contract)
    changed["agent"]["cu_count"] = 39
    actual.write_text(json.dumps(changed), encoding="utf-8")
    completed = subprocess.run(
        [
            sys.executable,
            str(TOOL_PATH),
            "compare",
            "--expected",
            str(CONTRACT_PATH),
            "--actual",
            str(actual),
        ],
        text=True,
        capture_output=True,
        check=False,
    )
    assert completed.returncode == 1
    assert "agent:" in completed.stderr
    assert "pmc_contract_compare=failed differences=1" in completed.stderr
