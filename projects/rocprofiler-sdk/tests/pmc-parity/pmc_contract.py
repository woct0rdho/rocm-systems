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

"""Capture and compare the gfx1151 dispatch-PMC semantic contract.

The capture input is intentionally the public rocprofv3/rocprofv3-avail text and
output files. It does not import implementation-private Linux modules, so the
same utility can capture a future installed Windows implementation.
"""

from __future__ import annotations

import argparse
import ast
import csv
import json
import re
import sys
from collections import Counter
from pathlib import Path
from typing import Any

SCHEMA_VERSION = 1
_COUNTER_LINE = re.compile(r"^([A-Za-z_]+)\s*:\s*\t?(.*)$")
_DIMENSION = re.compile(r"([A-Za-z0-9_]+)\[(\d+):(\d+)\]")
_VERSION_LINE = re.compile(r"^\s*([A-Za-z0-9_]+):\s*(.*?)\s*$")

AGENT_CONTRACT_KEYS = (
    "name",
    "product_name",
    "node_id",
    "logical_node_id",
    "logical_node_type_id",
    "gfx_target_version",
    "wave_front_size",
    "num_xcc",
    "cu_count",
    "array_count",
    "num_shader_banks",
    "simd_arrays_per_engine",
    "cu_per_simd_array",
    "simd_per_cu",
    "simd_count",
    "max_waves_per_simd",
)


def _display_key(key: str) -> str:
    return key.strip().lower()


def _parse_scalar(value: str) -> Any:
    value = value.strip()
    try:
        return ast.literal_eval(value)
    except (SyntaxError, ValueError):
        return value


def parse_counter_info(text: str) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    """Parse ``rocprofv3-avail info --pmc`` output."""

    gpu_match = re.search(r"^GPU:(\d+)\s*$", text, re.MULTILINE)
    name_match = re.search(r"^Name:(\S+)\s*$", text, re.MULTILINE)
    if gpu_match is None or name_match is None:
        raise ValueError("counter info does not identify a GPU and architecture")

    records: list[dict[str, Any]] = []
    current: dict[str, Any] | None = None
    for line in text.splitlines():
        match = _COUNTER_LINE.match(line)
        if match is None:
            continue
        key, value = match.groups()
        if key == "Counter_Name":
            current = {"name": value.strip()}
            records.append(current)
            continue
        if current is None:
            continue
        normalized = _display_key(key)
        if normalized == "dimensions":
            dimensions = [
                {"name": name, "minimum": int(minimum), "maximum": int(maximum)}
                for name, minimum, maximum in _DIMENSION.findall(value)
            ]
            if not dimensions and value.strip():
                raise ValueError(f"could not parse counter dimensions: {value}")
            current[normalized] = dimensions
        else:
            current[normalized] = value.strip()

    if not records:
        raise ValueError("counter info contains no counters")

    names = [record["name"] for record in records]
    duplicates = sorted(name for name, count in Counter(names).items() if count != 1)
    if duplicates:
        raise ValueError(f"counter names are not unique: {', '.join(duplicates)}")

    for record in records:
        if "block" in record:
            record["kind"] = "raw"
        elif "expression" in record:
            record["kind"] = "derived"
        else:
            record["kind"] = "constant"
        record.setdefault("dimensions", [])
        record.setdefault("spm", "Not Supported")

    records.sort(key=lambda item: item["name"])
    return (
        {"logical_gpu_index": int(gpu_match.group(1)), "architecture": name_match.group(1)},
        records,
    )


def parse_agent_info(text: str) -> dict[str, Any]:
    """Parse the GPU section from ``rocprofv3-avail list --agent``."""

    gpu_match = re.search(r"^GPU:(\d+)\s*$", text, re.MULTILINE)
    if gpu_match is None:
        raise ValueError("agent info does not identify a GPU")

    result: dict[str, Any] = {"logical_gpu_index": int(gpu_match.group(1))}
    for line in text.splitlines():
        match = _COUNTER_LINE.match(line)
        if match is None:
            continue
        key, value = match.groups()
        normalized = _display_key(key)
        if normalized in AGENT_CONTRACT_KEYS:
            result[normalized] = _parse_scalar(value)

    missing = sorted(set(AGENT_CONTRACT_KEYS) - result.keys())
    if missing:
        raise ValueError(f"agent info is missing: {', '.join(missing)}")
    return result


def parse_version(text: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in text.splitlines():
        match = _VERSION_LINE.match(line)
        if match:
            result[match.group(1)] = match.group(2)
    if "version" not in result or "git_revision" not in result:
        raise ValueError("rocprofv3 version output is incomplete")
    return result


def parse_cli_contract(text: str) -> dict[str, Any]:
    format_match = re.search(r"--output-format\s+\{([^}]+)\}", text)
    if format_match is None:
        raise ValueError("rocprofv3 help does not describe output formats")
    required_switches = (
        "--pmc",
        "--input",
        "--output-format",
        "--kernel-include-regex",
        "--kernel-exclude-regex",
        "--kernel-iteration-range",
        "--selected-regions",
        "--list-avail",
    )
    missing = [switch for switch in required_switches if switch not in text]
    if missing:
        raise ValueError(f"rocprofv3 help is missing: {', '.join(missing)}")
    pmc_line = next(line.strip() for line in text.splitlines() if line.strip().startswith("--pmc"))
    return {
        "output_formats": format_match.group(1).split(","),
        "required_switches": list(required_switches),
        "multiple_pass_syntax": "repeated --pmc",
        "pmc_help": pmc_line,
    }


def _csv_summary(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8", newline="") as input_file:
        reader = csv.DictReader(input_file)
        rows = list(reader)
        fields = reader.fieldnames or []
    summary: dict[str, Any] = {"fields": fields, "rows": len(rows)}
    if "Counter_Name" in fields:
        summary.update(
            {
                "counter_names": sorted({row["Counter_Name"] for row in rows}),
                "dispatch_ids": sorted({int(row["Dispatch_Id"]) for row in rows}),
                "kernel_names": sorted({row["Kernel_Name"] for row in rows}),
            }
        )
        sq_vector_values = sorted(
            {
                float(row["Counter_Value"])
                for row in rows
                if row["Counter_Name"] == "SQ_WAVES" and "vector_add" in row["Kernel_Name"]
            }
        )
        if sq_vector_values:
            summary["vector_add_sq_waves_values"] = sq_vector_values
    return summary


def summarize_behavior(root: Path, commands: dict[str, str]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for case_dir in sorted(path for path in root.iterdir() if path.is_dir()):
        status_path = case_dir / "status.txt"
        files_path = case_dir / "files.txt"
        if not status_path.is_file() or not files_path.is_file():
            raise ValueError(f"behavior case is incomplete: {case_dir}")
        name = case_dir.name
        case: dict[str, Any] = {
            "command": commands.get(name, ""),
            "exit_status": int(status_path.read_text(encoding="utf-8").strip()),
            "files": [
                line.strip().replace("\\", "/")
                for line in files_path.read_text(encoding="utf-8").splitlines()
                if line.strip()
            ],
            "counter_outputs": {},
        }
        output_root = case_dir / "output"
        for counter_csv in sorted(output_root.rglob("*_counter_collection.csv")):
            relative = counter_csv.relative_to(output_root).as_posix()
            case["counter_outputs"][relative] = _csv_summary(counter_csv)
        for results_json in sorted(output_root.rglob("*_results.json")):
            data = json.loads(results_json.read_text(encoding="utf-8"))
            tool = data["rocprofiler-sdk-tool"][0]
            relative = results_json.relative_to(output_root).as_posix()
            case.setdefault("json_outputs", {})[relative] = {
                "top_level_sections": sorted(tool),
                "callback_categories": sorted(tool["callback_records"]),
                "buffer_categories": sorted(tool["buffer_records"]),
            }
        result[name] = case
    return result


def build_contract(args: argparse.Namespace) -> dict[str, Any]:
    identity, counters = parse_counter_info(args.counter_info.read_text(encoding="utf-8"))
    agent = parse_agent_info(args.agent_info.read_text(encoding="utf-8"))
    if identity["logical_gpu_index"] != agent["logical_gpu_index"]:
        raise ValueError("counter and agent information refer to different GPUs")
    version = parse_version(args.version.read_text(encoding="utf-8"))
    commands = json.loads(args.behavior_commands.read_text(encoding="utf-8"))
    profile_observations = json.loads(args.profile_observations.read_text(encoding="utf-8"))
    kinds = Counter(counter["kind"] for counter in counters)
    return {
        "schema_version": SCHEMA_VERSION,
        "oracle": {
            "repository_revision": args.revision.read_text(encoding="utf-8").strip(),
            "repository_branch": args.branch.read_text(encoding="utf-8").strip(),
            "rocprofv3": version,
        },
        "agent": agent,
        "counter_catalog": {
            "architecture": identity["architecture"],
            "count": len(counters),
            "kind_counts": dict(sorted(kinds.items())),
            "counters": counters,
        },
        "cli": parse_cli_contract(args.rocprofv3_help.read_text(encoding="utf-8")),
        "profile_observations": profile_observations,
        "behavior": summarize_behavior(args.behavior_root, commands),
        "comparison_normalization": {
            "ignored_provenance": ["repository_revision", "repository_branch", "rocprofv3"],
            "ignored_agent_fields": ["product_name"],
            "ignored_record_values": [
                "Process_Id",
                "Thread_Id",
                "Start_Timestamp",
                "End_Timestamp",
                "Counter_Value",
            ],
            "canonicalized_paths": True,
        },
        "semantic_assertions": {
            "vector_add_grid_size": 1048576,
            "vector_add_workgroup_size": 256,
            "vector_add_iterations": 8,
            "vector_add_sq_waves": 32768.0,
            "counter_records_correlate_with_kernel_dispatch": True,
            "multipass_dispatch_ids_repeat_per_pass": True,
            "no_dispatch_success_emits_no_files": True,
            "target_status_is_preserved_without_output": True,
            "unknown_counter_warns_and_preserves_successful_target_status": True,
        },
    }


def compare_contracts(expected: dict[str, Any], actual: dict[str, Any]) -> list[str]:
    """Return human-readable semantic differences.

    Provenance and observed numeric counter values are evidence, not equality
    inputs. Architecture, topology, catalog metadata, CLI, output structure,
    statuses, identities, and semantic assertions are equality inputs.
    """

    differences: list[str] = []

    def compare(path: str, left: Any, right: Any) -> None:
        if left != right:
            differences.append(f"{path}: expected {left!r}, got {right!r}")

    compare("schema_version", expected.get("schema_version"), actual.get("schema_version"))
    compare(
        "comparison_normalization",
        expected.get("comparison_normalization"),
        actual.get("comparison_normalization"),
    )
    ignored_agent_fields = set(
        expected.get("comparison_normalization", {}).get("ignored_agent_fields", [])
    )
    expected_agent = {
        key: value for key, value in expected.get("agent", {}).items() if key not in ignored_agent_fields
    }
    actual_agent = {
        key: value for key, value in actual.get("agent", {}).items() if key not in ignored_agent_fields
    }
    compare("agent", expected_agent, actual_agent)

    expected_catalog = expected.get("counter_catalog", {})
    actual_catalog = actual.get("counter_catalog", {})
    for key in ("architecture", "count", "kind_counts"):
        compare(f"counter_catalog.{key}", expected_catalog.get(key), actual_catalog.get(key))
    expected_counters = {item["name"]: item for item in expected_catalog.get("counters", [])}
    actual_counters = {item["name"]: item for item in actual_catalog.get("counters", [])}
    compare("counter_catalog.names", sorted(expected_counters), sorted(actual_counters))
    for counter_name in sorted(set(expected_counters) & set(actual_counters)):
        compare(
            f"counter_catalog.counters.{counter_name}",
            expected_counters[counter_name],
            actual_counters[counter_name],
        )

    compare("cli", expected.get("cli"), actual.get("cli"))
    compare(
        "profile_observations",
        expected.get("profile_observations"),
        actual.get("profile_observations"),
    )
    compare("semantic_assertions", expected.get("semantic_assertions"), actual.get("semantic_assertions"))

    expected_behavior = expected.get("behavior", {})
    actual_behavior = actual.get("behavior", {})
    compare("behavior cases", sorted(expected_behavior), sorted(actual_behavior))
    for case_name in sorted(set(expected_behavior) & set(actual_behavior)):
        expected_case = expected_behavior[case_name]
        actual_case = actual_behavior[case_name]
        for key in ("command", "exit_status", "files"):
            compare(f"behavior.{case_name}.{key}", expected_case.get(key), actual_case.get(key))
        compare(
            f"behavior.{case_name}.json_outputs",
            expected_case.get("json_outputs", {}),
            actual_case.get("json_outputs", {}),
        )
        expected_outputs = expected_case.get("counter_outputs", {})
        actual_outputs = actual_case.get("counter_outputs", {})
        compare(
            f"behavior.{case_name}.counter output names",
            sorted(expected_outputs),
            sorted(actual_outputs),
        )
        for output_name in sorted(set(expected_outputs) & set(actual_outputs)):
            expected_output = expected_outputs[output_name]
            actual_output = actual_outputs[output_name]
            for key in ("fields", "rows", "counter_names", "dispatch_ids", "kernel_names"):
                compare(
                    f"behavior.{case_name}.{output_name}.{key}",
                    expected_output.get(key),
                    actual_output.get(key),
                )
    return differences


def _load_contract(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"contract must be a JSON object: {path}")
    return data


def _capture_parser(subparsers: argparse._SubParsersAction) -> None:
    parser = subparsers.add_parser("capture", help="capture a contract from installed-tool output")
    parser.add_argument("--counter-info", type=Path, required=True)
    parser.add_argument("--agent-info", type=Path, required=True)
    parser.add_argument("--rocprofv3-help", type=Path, required=True)
    parser.add_argument("--version", type=Path, required=True)
    parser.add_argument("--revision", type=Path, required=True)
    parser.add_argument("--branch", type=Path, required=True)
    parser.add_argument("--behavior-root", type=Path, required=True)
    parser.add_argument("--behavior-commands", type=Path, required=True)
    parser.add_argument("--profile-observations", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)


def _compare_parser(subparsers: argparse._SubParsersAction) -> None:
    parser = subparsers.add_parser("compare", help="compare an actual capture with the oracle")
    parser.add_argument("--expected", type=Path, required=True)
    parser.add_argument("--actual", type=Path, required=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    _capture_parser(subparsers)
    _compare_parser(subparsers)
    args = parser.parse_args(argv)

    try:
        if args.command == "capture":
            contract = build_contract(args)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_bytes((json.dumps(contract, indent=2) + "\n").encode("utf-8"))
            print(
                "pmc_contract_capture=passed "
                f"architecture={contract['counter_catalog']['architecture']} "
                f"counters={contract['counter_catalog']['count']}"
            )
            return 0
        expected = _load_contract(args.expected)
        actual = _load_contract(args.actual)
        differences = compare_contracts(expected, actual)
        if differences:
            for difference in differences:
                print(difference, file=sys.stderr)
            print(f"pmc_contract_compare=failed differences={len(differences)}", file=sys.stderr)
            return 1
        print(
            "pmc_contract_compare=passed "
            f"architecture={expected['counter_catalog']['architecture']} "
            f"counters={expected['counter_catalog']['count']}"
        )
        return 0
    except (KeyError, OSError, ValueError, json.JSONDecodeError) as error:
        print(f"pmc_contract_error={error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
