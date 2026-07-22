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
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

from __future__ import annotations

import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import sqlite3
import subprocess
import uuid


SCHEMA_MANIFEST = "latest-schema.json"
SCHEMA_KEYS = (
    "rocpd_metadata",
    "rocpd_tables",
    "rocpd_views",
    "rocpd_indexes",
    "rocpd_data_views",
    "rocpd_summary_views",
)
SCHEMA_EXECUTION_ORDER = (
    "rocpd_tables",
    "rocpd_indexes",
    "rocpd_views",
    "rocpd_data_views",
    "rocpd_summary_views",
    "rocpd_metadata",
)
_VERSION_PATTERN = re.compile(r"^([0-9]+)\.([0-9]+)\.([0-9]+)$")
_VERSION_LINE_PATTERN = re.compile(
    r'^    - version: "([0-9]+\.[0-9]+\.[0-9]+)"$'
)
_SCHEMA_LINE_PATTERN = re.compile(r"^      ([a-z_]+): +([^ ]+)$")


class RocpdConversionError(RuntimeError):
    pass


def _version_parts(value, field):
    match = _VERSION_PATTERN.fullmatch(value) if isinstance(value, str) else None
    if match is None:
        raise RocpdConversionError(f"invalid {field}: {value!r}")
    parts = tuple(int(item) for item in match.groups())
    if parts[1] > 99 or parts[2] > 99:
        raise RocpdConversionError(
            f"{field} components cannot be represented as a SQLite user_version: {value}"
        )
    if value != ".".join(str(item) for item in parts):
        raise RocpdConversionError(f"non-canonical {field}: {value}")
    return parts


def _versions_configuration(path):
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as error:
        raise RocpdConversionError(f"could not read {path}: {error}") from error
    entries = {}
    current = None
    for line in lines:
        version_match = _VERSION_LINE_PATTERN.fullmatch(line)
        if version_match:
            current = version_match.group(1)
            if current in entries:
                raise RocpdConversionError(f"duplicate ROCpd schema version {current}")
            entries[current] = {}
            continue
        schema_match = _SCHEMA_LINE_PATTERN.fullmatch(line)
        if current is not None and schema_match and schema_match.group(1) in SCHEMA_KEYS:
            key, value = schema_match.groups()
            if key in entries[current]:
                raise RocpdConversionError(
                    f"duplicate {key} asset for ROCpd schema {current}"
                )
            entries[current][key] = value
    if not entries:
        raise RocpdConversionError(f"no ROCpd schema versions are declared in {path}")
    latest = max(entries, key=lambda value: _version_parts(value, "schema version"))
    return latest, entries[latest]


def _manifest_integer(manifest, field):
    value = manifest.get(field)
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise RocpdConversionError(f"invalid {field} in {SCHEMA_MANIFEST}: {value!r}")
    return value


def _load_schema_configuration(directory):
    directory = Path(directory).resolve()
    manifest_path = directory / SCHEMA_MANIFEST
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise RocpdConversionError(f"could not read {manifest_path}: {error}") from error
    if not isinstance(manifest, dict) or manifest.get("format") != 1:
        raise RocpdConversionError(f"invalid ROCpd schema manifest format in {manifest_path}")

    version = manifest.get("version")
    parts = _version_parts(version, "schema manifest version")
    manifest_parts = tuple(
        _manifest_integer(manifest, field) for field in ("major", "minor", "patch")
    )
    if manifest_parts != parts:
        raise RocpdConversionError(
            f"inconsistent version components in {manifest_path}: {manifest_parts} != {parts}"
        )
    user_version = _manifest_integer(manifest, "user_version")
    expected_user_version = parts[0] * 10000 + parts[1] * 100 + parts[2]
    if user_version != expected_user_version:
        raise RocpdConversionError(
            f"inconsistent user_version in {manifest_path}: "
            f"{user_version} != {expected_user_version}"
        )

    schema_files = manifest.get("schema_files")
    if not isinstance(schema_files, dict) or set(schema_files) != set(SCHEMA_KEYS):
        raise RocpdConversionError(
            f"incomplete schema_files mapping in {manifest_path}"
        )
    latest_version, latest_files = _versions_configuration(directory / "versions.yml")
    if version != latest_version or schema_files != latest_files:
        raise RocpdConversionError(
            f"schema manifest does not match the latest versions.yml entry in {directory}"
        )
    if len(set(schema_files.values())) != len(schema_files):
        raise RocpdConversionError(f"schema assets are not unique in {manifest_path}")

    resolved_files = {}
    for key, name in schema_files.items():
        if not isinstance(name, str) or not name:
            raise RocpdConversionError(f"invalid {key} asset in {manifest_path}: {name!r}")
        asset = (directory / name).resolve()
        try:
            asset.relative_to(directory)
        except ValueError as error:
            raise RocpdConversionError(
                f"schema asset escapes its schema directory: {name}"
            ) from error
        if not asset.is_file():
            raise RocpdConversionError(f"schema asset does not exist: {asset}")
        resolved_files[key] = asset

    return {
        "directory": directory,
        "version": version,
        "version_parts": parts,
        "user_version": user_version,
        "files": tuple(resolved_files[key] for key in SCHEMA_EXECUTION_ORDER),
    }


def schema_configuration(configured=None):
    configured = configured or os.environ.get("ROCPD_SCHEMA_PATH")
    candidates = (
        [Path(configured)]
        if configured
        else [
            Path(__file__).resolve().parent.parent
            / "share"
            / "rocprofiler-sdk-rocpd"
        ]
    )
    errors = []
    for candidate in candidates:
        try:
            return _load_schema_configuration(candidate)
        except RocpdConversionError as error:
            errors.append(str(error))
    raise RocpdConversionError(
        "could not locate a complete ROCpd schema configuration: " + "; ".join(errors)
    )


def schema_directory(configured=None):
    return schema_configuration(configured)["directory"]


def _integer(value, field):
    if isinstance(value, dict):
        for key in ("handle", "value", "internal", "external"):
            if key in value:
                return _integer(value[key], field)
    try:
        return int(value)
    except (TypeError, ValueError, OverflowError) as error:
        raise RocpdConversionError(
            f"invalid integer field {field}: {value!r}"
        ) from error


def _required(mapping, field):
    if not isinstance(mapping, dict) or field not in mapping:
        raise RocpdConversionError(f"authoritative JSON is missing {field}")
    return mapping[field]


def _json(value):
    return json.dumps(value, separators=(",", ":"), sort_keys=True)


def _render_schema(path, suffix, guid, schema):
    contents = path.read_text(encoding="utf-8")
    major, minor, patch = schema["version_parts"]
    replacements = {
        "{{uuid}}": suffix,
        "{{guid}}": guid,
        "{{schema_version}}": schema["version"],
        "{{schema_version_major}}": str(major),
        "{{schema_version_minor}}": str(minor),
        "{{schema_version_patch}}": str(patch),
    }
    for key, value in replacements.items():
        contents = contents.replace(key, value)
    if "{{" in contents or "}}" in contents:
        raise RocpdConversionError(f"unresolved schema variable in {path}")
    return contents


def _load_document(source):
    try:
        document = json.loads(Path(source).read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise RocpdConversionError(
            f"could not read authoritative JSON {source}: {error}"
        ) from error
    tools = document.get("rocprofiler-sdk-tool") if isinstance(document, dict) else None
    if isinstance(tools, dict):
        tools = [tools]
    if not isinstance(tools, list) or len(tools) != 1 or not isinstance(tools[0], dict):
        raise RocpdConversionError(
            "authoritative JSON must contain exactly one rocprofiler-sdk-tool document"
        )
    return tools[0]


def _dispatch_records(tool):
    buffer_records = tool.get("buffer_records", {})
    records = buffer_records.get("kernel_dispatch", [])
    if records:
        return records
    output = []
    callback_records = tool.get("callback_records", {})
    for counter in callback_records.get("counter_collection", []):
        dispatch = dict(_required(counter, "dispatch_data"))
        dispatch["thread_id"] = _required(counter, "thread_id")
        dispatch["stream_id"] = counter.get("stream_id", {"handle": 0})
        dispatch["graph_exec_id"] = 0
        dispatch["graph_node_id"] = 0
        output.append(dispatch)
    return output


def _normalized_lds(value):
    value = _integer(value, "group_segment_size")
    if value < 0:
        raise RocpdConversionError("group_segment_size must not be negative")
    return (value + 511) & ~511


def _validate_symbol(symbol):
    kernel_id = _integer(_required(symbol, "kernel_id"), "kernel_id")
    name = str(_required(symbol, "formatted_kernel_name"))
    kernel_object = _integer(_required(symbol, "kernel_object"), "kernel_object")
    kernel_address = _integer(
        _required(_required(symbol, "kernel_address"), "handle"), "kernel_address"
    )
    arch_vgpr_count = _integer(_required(symbol, "arch_vgpr_count"), "arch_vgpr_count")
    sgpr_count = _integer(_required(symbol, "sgpr_count"), "sgpr_count")
    if kernel_id <= 0 or not name or kernel_object <= 0 or kernel_address <= 0:
        raise RocpdConversionError(
            f"kernel_metadata_missing: invalid identity for kernel {kernel_id}"
        )
    if arch_vgpr_count <= 0 or sgpr_count <= 0:
        raise RocpdConversionError(
            f"kernel_metadata_missing: invalid register counts for kernel {kernel_id}"
        )
    for field in ("private_segment_size", "accum_vgpr_count"):
        if _integer(_required(symbol, field), field) < 0:
            raise RocpdConversionError(
                f"kernel_metadata_missing: negative {field} for kernel {kernel_id}"
            )
    _normalized_lds(_required(symbol, "group_segment_size"))
    return kernel_id


def _prepare_records(tool):
    symbols = {}
    for symbol in tool.get("kernel_symbols", []):
        kernel_id = _validate_symbol(symbol)
        if kernel_id in symbols:
            raise RocpdConversionError(f"duplicate kernel symbol ID {kernel_id}")
        symbols[kernel_id] = symbol

    dispatches = {}
    for record in _dispatch_records(tool):
        info = _required(record, "dispatch_info")
        dispatch_id = _integer(_required(info, "dispatch_id"), "dispatch_id")
        kernel_id = _integer(_required(info, "kernel_id"), "kernel_id")
        start = _integer(_required(record, "start_timestamp"), "start_timestamp")
        end = _integer(_required(record, "end_timestamp"), "end_timestamp")
        if dispatch_id <= 0 or dispatch_id in dispatches:
            raise RocpdConversionError(
                f"invalid or duplicate dispatch ID {dispatch_id}"
            )
        if kernel_id not in symbols:
            raise RocpdConversionError(
                f"kernel_metadata_missing: dispatch {dispatch_id} references kernel {kernel_id}"
            )
        if start <= 0 or end <= start:
            raise RocpdConversionError(
                f"dispatch {dispatch_id} has invalid authoritative timestamps {start}:{end}"
            )
        for dimensions in ("workgroup_size", "grid_size"):
            value = _required(info, dimensions)
            if any(
                _integer(_required(value, axis), f"{dimensions}.{axis}") <= 0
                for axis in "xyz"
            ):
                raise RocpdConversionError(
                    f"dispatch {dispatch_id} has invalid {dimensions}"
                )
        dispatches[dispatch_id] = record

    counters = {}
    callback_records = tool.get("callback_records", {})
    for record in callback_records.get("counter_collection", []):
        info = _required(_required(record, "dispatch_data"), "dispatch_info")
        dispatch_id = _integer(_required(info, "dispatch_id"), "dispatch_id")
        if dispatch_id in counters:
            raise RocpdConversionError(
                f"duplicate counter record for dispatch {dispatch_id}"
            )
        counters[dispatch_id] = record
    if counters and set(counters) != set(dispatches):
        raise RocpdConversionError(
            "counter and kernel dispatch IDs differ: "
            f"counters={sorted(counters)} dispatches={sorted(dispatches)}"
        )
    return symbols, dispatches, counters


def _insert_document(connection, suffix, tool, command):
    metadata = tool.get("metadata", {})
    process_id = _integer(_required(metadata, "pid"), "metadata.pid")
    init_time = _integer(metadata.get("init_time", 0), "metadata.init_time")
    fini_time = _integer(metadata.get("fini_time", 0), "metadata.fini_time")
    symbols, dispatches, counter_records = _prepare_records(tool)

    agents = tool.get("agents", [])
    if not agents:
        raise RocpdConversionError("authoritative JSON contains no agents")
    agent_handles = {}
    gpu_agent_id = None
    for index, agent in enumerate(agents):
        node_id = _integer(agent.get("node_id", index), "agent.node_id")
        handle = _integer(_required(_required(agent, "id"), "handle"), "agent.id")
        if handle in agent_handles:
            raise RocpdConversionError(f"duplicate agent handle {handle}")
        agent_handles[handle] = node_id
        if _integer(agent.get("type", 0), "agent.type") == 2 and gpu_agent_id is None:
            gpu_agent_id = node_id
    if gpu_agent_id is None:
        raise RocpdConversionError("authoritative JSON contains no GPU agent")

    machine_id = platform.node() or "windows-host"
    node_hash = (
        int.from_bytes(
            hashlib.sha256(machine_id.encode("utf-8")).digest()[:8], "little"
        )
        & 0x7FFFFFFFFFFFFFFF
    )
    node_table = f"rocpd_info_node{suffix}"
    process_table = f"rocpd_info_process{suffix}"
    agent_table = f"rocpd_info_agent{suffix}"
    connection.execute(
        f'INSERT INTO "{node_table}" '
        "(id, hash, machine_id, system_name, hostname, release, version, hardware_name, domain_name) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
        (
            1,
            node_hash,
            machine_id,
            platform.system() or "Windows",
            machine_id,
            platform.release(),
            platform.version(),
            platform.machine(),
            "",
        ),
    )
    command_text = (
        subprocess.list2cmdline([str(value) for value in command]) if command else ""
    )
    connection.execute(
        f'INSERT INTO "{process_table}" '
        "(id, nid, ppid, pid, init, fini, start, end, command, environment, extdata) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        (
            process_id,
            1,
            os.getpid(),
            process_id,
            init_time,
            fini_time,
            init_time,
            fini_time,
            command_text,
            "{}",
            _json(
                {
                    "producer": "rocprofv3-windows-post-target",
                    "source": "rocprofiler-sdk-tool-json",
                }
            ),
        ),
    )
    for index, agent in enumerate(agents):
        node_id = _integer(agent.get("node_id", index), "agent.node_id")
        type_id = _integer(agent.get("type", 0), "agent.type")
        agent_type = {1: "CPU", 2: "GPU"}.get(type_id)
        if agent_type is None:
            raise RocpdConversionError(f"unsupported agent type {type_id}")
        connection.execute(
            f'INSERT INTO "{agent_table}" '
            "(id, nid, pid, type, absolute_index, logical_index, type_index, uuid, name, "
            "model_name, vendor_name, product_name, user_name, extdata) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (
                node_id,
                1,
                process_id,
                agent_type,
                node_id,
                _integer(
                    agent.get("logical_node_id", node_id), "agent.logical_node_id"
                ),
                _integer(
                    agent.get("logical_node_type_id", index),
                    "agent.logical_node_type_id",
                ),
                _integer(agent.get("device_id", 0), "agent.device_id"),
                str(agent.get("name", "")),
                str(agent.get("model_name", "")),
                str(agent.get("vendor_name", "")),
                str(agent.get("product_name", "")),
                str(agent.get("product_name", "")),
                _json(agent),
            ),
        )

    string_table = f"rocpd_string{suffix}"
    connection.executemany(
        f'INSERT INTO "{string_table}" (id, string) VALUES (?, ?)',
        ((1, "KERNEL_DISPATCH"), (2, "")),
    )

    thread_ids = sorted(
        {
            _integer(_required(record, "thread_id"), "thread_id")
            for record in dispatches.values()
        }
    )
    thread_table = f"rocpd_info_thread{suffix}"
    connection.executemany(
        f'INSERT INTO "{thread_table}" (id, nid, ppid, pid, tid) VALUES (?, ?, ?, ?, ?)',
        ((tid, 1, os.getpid(), process_id, tid) for tid in thread_ids),
    )

    queue_ids = sorted(
        {
            _integer(
                _required(
                    _required(_required(record, "dispatch_info"), "queue_id"), "handle"
                ),
                "queue_id",
            )
            for record in dispatches.values()
        }
    )
    queue_table = f"rocpd_info_queue{suffix}"
    connection.executemany(
        f'INSERT INTO "{queue_table}" (id, nid, pid, name) VALUES (?, ?, ?, ?)',
        ((queue_id, 1, process_id, f"Queue {queue_id}") for queue_id in queue_ids),
    )
    stream_ids = sorted(
        {
            _integer(record.get("stream_id", {"handle": 0}), "stream_id")
            for record in dispatches.values()
        }
    )
    stream_table = f"rocpd_info_stream{suffix}"
    connection.executemany(
        f'INSERT INTO "{stream_table}" (id, nid, pid, name) VALUES (?, ?, ?, ?)',
        (
            (
                stream_id,
                1,
                process_id,
                "Default Stream" if stream_id == 0 else f"Stream {stream_id}",
            )
            for stream_id in stream_ids
        ),
    )

    code_object_table = f"rocpd_info_code_object{suffix}"
    code_object_ids = sorted(
        {
            _integer(symbol.get("code_object_id", 0), "code_object_id")
            for symbol in symbols.values()
        }
    )
    connection.executemany(
        f'INSERT INTO "{code_object_table}" '
        "(id, nid, pid, agent_id, uri, load_base, load_size, load_delta, storage_type, extdata) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        (
            (
                code_object_id,
                1,
                process_id,
                gpu_agent_id,
                "",
                0,
                0,
                0,
                "MEMORY",
                _json({"windows_placeholder": True, "code_object_id": code_object_id}),
            )
            for code_object_id in code_object_ids
        ),
    )

    symbol_table = f"rocpd_info_kernel_symbol{suffix}"
    for kernel_id, symbol in sorted(symbols.items()):
        connection.execute(
            f'INSERT INTO "{symbol_table}" '
            "(id, nid, pid, code_object_id, kernel_name, display_name, kernel_object, "
            "kernarg_segment_size, kernarg_segment_alignment, group_segment_size, "
            "private_segment_size, sgpr_count, arch_vgpr_count, accum_vgpr_count, extdata) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (
                kernel_id,
                1,
                process_id,
                _integer(symbol.get("code_object_id", 0), "code_object_id"),
                str(symbol.get("kernel_name", "")),
                str(_required(symbol, "formatted_kernel_name")),
                _integer(_required(symbol, "kernel_object"), "kernel_object"),
                _integer(symbol.get("kernarg_segment_size", 0), "kernarg_segment_size"),
                _integer(
                    symbol.get("kernarg_segment_alignment", 0),
                    "kernarg_segment_alignment",
                ),
                _normalized_lds(_required(symbol, "group_segment_size")),
                _integer(
                    _required(symbol, "private_segment_size"), "private_segment_size"
                ),
                _integer(_required(symbol, "sgpr_count"), "sgpr_count"),
                _integer(_required(symbol, "arch_vgpr_count"), "arch_vgpr_count"),
                _integer(_required(symbol, "accum_vgpr_count"), "accum_vgpr_count"),
                _json(symbol),
            ),
        )

    counter_table = f"rocpd_info_pmc{suffix}"
    counter_ids = set()
    counters = tool.get("counters", [])
    for counter in counters:
        counter_id = _integer(
            _required(_required(counter, "id"), "handle"), "counter.id"
        )
        if counter_id in counter_ids:
            raise RocpdConversionError(f"duplicate counter ID {counter_id}")
        counter_ids.add(counter_id)
        agent_handle = _integer(
            _required(_required(counter, "agent_id"), "handle"), "counter.agent_id"
        )
        if agent_handle not in agent_handles:
            raise RocpdConversionError(
                f"counter references unknown agent {agent_handle}"
            )
        name = str(_required(counter, "name"))
        connection.execute(
            f'INSERT INTO "{counter_table}" '
            "(id, nid, pid, agent_id, target_arch, name, symbol, description, component, "
            "value_type, block, expression, is_constant, is_derived, extdata) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (
                counter_id,
                1,
                process_id,
                agent_handles[agent_handle],
                "GPU",
                name,
                name,
                str(counter.get("description", "")),
                "rocm",
                "ABS",
                str(counter.get("block", "")),
                str(counter.get("expression", "")),
                _integer(counter.get("is_constant", 0), "counter.is_constant"),
                _integer(counter.get("is_derived", 0), "counter.is_derived"),
                _json(
                    next(
                        agent
                        for agent in agents
                        if _integer(agent["id"]["handle"], "agent.id") == agent_handle
                    )
                ),
            ),
        )

    event_table = f"rocpd_event{suffix}"
    dispatch_table = f"rocpd_kernel_dispatch{suffix}"
    for dispatch_id, record in sorted(dispatches.items()):
        info = _required(record, "dispatch_info")
        correlation = _required(record, "correlation_id")
        internal = _integer(correlation.get("internal", 0), "correlation_id.internal")
        external = _integer(correlation.get("external", 0), "correlation_id.external")
        connection.execute(
            f'INSERT INTO "{event_table}" '
            "(id, category_id, stack_id, parent_stack_id, correlation_id) "
            "VALUES (?, ?, ?, ?, ?)",
            (dispatch_id, 1, internal, internal, external),
        )
        agent_handle = _integer(
            _required(_required(info, "agent_id"), "handle"), "dispatch.agent_id"
        )
        if agent_handle not in agent_handles:
            raise RocpdConversionError(
                f"dispatch {dispatch_id} references unknown agent {agent_handle}"
            )
        workgroup = _required(info, "workgroup_size")
        grid = _required(info, "grid_size")
        connection.execute(
            f'INSERT INTO "{dispatch_table}" '
            "(id, nid, pid, tid, agent_id, kernel_id, dispatch_id, queue_id, stream_id, "
            "start, end, private_segment_size, group_segment_size, workgroup_size_x, "
            "workgroup_size_y, workgroup_size_z, grid_size_x, grid_size_y, grid_size_z, "
            "graph_exec_id, graph_node_id, region_name_id, event_id) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (
                dispatch_id,
                1,
                process_id,
                _integer(_required(record, "thread_id"), "thread_id"),
                agent_handles[agent_handle],
                _integer(_required(info, "kernel_id"), "kernel_id"),
                dispatch_id,
                _integer(_required(_required(info, "queue_id"), "handle"), "queue_id"),
                _integer(record.get("stream_id", {"handle": 0}), "stream_id"),
                _integer(_required(record, "start_timestamp"), "start_timestamp"),
                _integer(_required(record, "end_timestamp"), "end_timestamp"),
                _integer(
                    _required(info, "private_segment_size"), "private_segment_size"
                ),
                _normalized_lds(_required(info, "group_segment_size")),
                _integer(_required(workgroup, "x"), "workgroup_size.x"),
                _integer(_required(workgroup, "y"), "workgroup_size.y"),
                _integer(_required(workgroup, "z"), "workgroup_size.z"),
                _integer(_required(grid, "x"), "grid_size.x"),
                _integer(_required(grid, "y"), "grid_size.y"),
                _integer(_required(grid, "z"), "grid_size.z"),
                _integer(record.get("graph_exec_id", 0), "graph_exec_id"),
                _integer(record.get("graph_node_id", 0), "graph_node_id"),
                2,
                dispatch_id,
            ),
        )

    pmc_event_table = f"rocpd_pmc_event{suffix}"
    pmc_event_id = 1
    for dispatch_id, record in sorted(counter_records.items()):
        for counter in _required(record, "records"):
            counter_id = _integer(
                _required(_required(counter, "counter_id"), "handle"), "counter_id"
            )
            if counter_id not in counter_ids:
                raise RocpdConversionError(
                    f"dispatch {dispatch_id} references unknown counter {counter_id}"
                )
            try:
                value = float(_required(counter, "value"))
            except (TypeError, ValueError, OverflowError) as error:
                raise RocpdConversionError(
                    f"dispatch {dispatch_id} has invalid counter value"
                ) from error
            if not math.isfinite(value):
                raise RocpdConversionError(
                    f"dispatch {dispatch_id} has non-finite counter value"
                )
            connection.execute(
                f'INSERT INTO "{pmc_event_table}" (id, event_id, pmc_id, value) '
                "VALUES (?, ?, ?, ?)",
                (pmc_event_id, dispatch_id, counter_id, value),
            )
            pmc_event_id += 1

    metadata_table = f"rocpd_metadata{suffix}"
    connection.executemany(
        f'INSERT INTO "{metadata_table}" (tag, value) VALUES (?, ?)',
        (
            ("producer", "rocprofv3-windows-post-target"),
            ("source_format", "rocprofiler-sdk-tool-json"),
            ("dispatch_count", str(len(dispatches))),
            ("counter_record_count", str(pmc_event_id - 1)),
        ),
    )
    return {
        "dispatches": len(dispatches),
        "counters": pmc_event_id - 1,
        "kernel_symbols": len(symbols),
    }


def convert_json_to_rocpd(source, output, command=(), configured_schema=None):
    source = Path(source).resolve()
    output = Path(output).resolve()
    if output.exists():
        raise RocpdConversionError(f"output already exists: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    schema = schema_configuration(configured_schema)
    tool = _load_document(source)
    guid = str(uuid.uuid4())
    suffix = f"_windows_{guid.replace('-', '_')}"
    temporary = output.with_name(f".{output.name}.{uuid.uuid4().hex}.tmp")
    try:
        try:
            temporary.open("xb").close()
        except OSError as error:
            raise RocpdConversionError(
                f"could not reserve temporary database {temporary}: {error}"
            ) from error
        try:
            connection = sqlite3.connect(str(temporary), timeout=30.0)
            try:
                connection.execute("PRAGMA journal_mode=DELETE")
                connection.execute("PRAGMA foreign_keys=ON")
                connection.execute(f"PRAGMA user_version={schema['user_version']}")
                for path in schema["files"]:
                    connection.executescript(
                        _render_schema(path, suffix, guid, schema)
                    )
                with connection:
                    counts = _insert_document(connection, suffix, tool, command)
                actual_version = connection.execute(
                    "SELECT value FROM rocpd_metadata WHERE tag = 'schema_version'"
                ).fetchone()
                if actual_version != (schema["version"],):
                    raise RocpdConversionError(
                        "generated schema metadata does not match its manifest: "
                        f"{actual_version!r} != {schema['version']!r}"
                    )
                actual_user_version = connection.execute("PRAGMA user_version").fetchone()
                if actual_user_version != (schema["user_version"],):
                    raise RocpdConversionError(
                        "generated SQLite user_version does not match its manifest: "
                        f"{actual_user_version!r} != {schema['user_version']!r}"
                    )
                foreign_key_errors = connection.execute(
                    "PRAGMA foreign_key_check"
                ).fetchall()
                if foreign_key_errors:
                    raise RocpdConversionError(
                        f"generated database has foreign-key errors: {foreign_key_errors[:3]}"
                    )
                integrity = connection.execute("PRAGMA integrity_check").fetchone()
                if not integrity or integrity[0] != "ok":
                    raise RocpdConversionError(
                        f"generated database failed integrity_check: {integrity}"
                    )
            finally:
                connection.close()
        except RocpdConversionError:
            raise
        except (OSError, UnicodeError, sqlite3.Error) as error:
            raise RocpdConversionError(
                f"could not generate {output}: {error}"
            ) from error
        try:
            os.link(temporary, output)
        except FileExistsError as error:
            raise RocpdConversionError(f"output already exists: {output}") from error
        except OSError as error:
            raise RocpdConversionError(
                f"could not publish {output}: {error}"
            ) from error
        return counts
    finally:
        temporary.unlink(missing_ok=True)
