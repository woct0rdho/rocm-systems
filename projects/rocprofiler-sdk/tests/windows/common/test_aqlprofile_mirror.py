from __future__ import annotations

import os
from pathlib import Path
import re


def source(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def initializer(text: str, name: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s*=\s*(.*?);", text, re.DOTALL)
    assert match, f"missing {name}"
    value = re.sub(r"//.*", "", match.group(1))
    return re.sub(r"\s+", "", value)


def test_windows_aqlprofile_packet_metadata_and_bounds_match():
    repository = Path(os.environ["ROCPROFILER_TEST_REPOSITORY_ROOT"]).resolve()
    standalone_packet = source(
        repository / "projects/aqlprofile/src/core/amd_aql_pm4_ib_packet.h"
    )
    embedded_packet = source(
        repository
        / "projects/rocprofiler-sdk/source/lib/aqlprofile/core/amd_aql_pm4_ib_packet.h"
    )
    for name in (
        "AMD_AQL_PM4_IB_RESERVED_COUNT",
        "AMD_AQL_PM4_IB_MANIFEST_MAGIC",
        "AMD_AQL_PM4_IB_MANIFEST_VERSION",
        "AMD_AQL_PM4_IB_MANIFEST_FLAG_EVENT_IDS_VALIDATED",
        "AMD_AQL_PM4_IB_MANIFEST_FLAG_INSTANCES_VALIDATED",
        "AMD_AQL_PM4_IB_MANIFEST_REQUIRED_FLAGS",
    ):
        assert initializer(standalone_packet, name) == initializer(
            embedded_packet, name
        ), f"AQL Profile packet metadata drifted for {name}"

    standalone_bounds = source(
        repository / "projects/aqlprofile/gfxip/gfx11/gfx11_block_info.h"
    )
    embedded_bounds = source(
        repository
        / "projects/rocprofiler-sdk/source/lib/aqlprofile/gfxip/gfx11/gfx11_block_info.h"
    )
    for name in (
        "GrbmCounterBlockMaxEvent",
        "GrbmSeCounterBlockMaxEvent",
        "GceaCounterBlockMaxEvent",
    ):
        assert initializer(standalone_bounds, name) == initializer(
            embedded_bounds, name
        ), f"gfx11 descriptor bound drifted for {name}"


def test_wddm_profile_frame_capacity_remains_qualified():
    repository = Path(os.environ["ROCPROFILER_TEST_REPOSITORY_ROOT"]).resolve()
    policy = source(
        repository
        / "projects/rocr-runtime/libhsakmt/include/impl/wddm/profiling.h"
    )
    assert initializer(policy, "kQualifiedFrameBytes") == "0x2000"
    assert initializer(policy, "kFrameTrailerReserveBytes") == "0x100"
    assert initializer(policy, "kMaximumPm4Dwords") == (
        "(kQualifiedFrameBytes-kFrameTrailerReserveBytes)/sizeof(uint32_t)"
    )
