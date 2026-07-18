# General and Host-Trap PC Sampling on gfx1151 (Strix Halo)

## Current Status

- End-to-end host-trap collection works.
- Session restart works, including restoration of host/device double-buffer parity for every XCC.
- Instructions resolve correctly in emitted samples.
- ROCr unit tests pass for PC sampling extension/config/lifecycle and KFD capability paths.
- ROCProfiler unit tests pass for gfx1151 parser and PC sampling service paths.
- General SDK PC-sampling support is intentionally narrowed to gfx1151 instead of overclaiming all gfx11xx targets.
- The gfx1151 ROCr handler captures HW ID with the named `HW_REG_HW_ID1` register rather than raw numeric `hwreg(24)`, which disassembles as `HW_REG_HW_ID2` on gfx11.x.
- rocpd PC-sampling output is published in formal schema `3.0.4`; upstream's SPM schema `3.0.3` and the earlier compatibility snapshots remain unchanged. A required `method` discriminator lets host-trap and stochastic rows share `rocpd_pc_sampling` without ambiguity. Stochastic rows additionally store the decoded CSV-equivalent fields `wave_issued_instruction`, `instruction_type`, `stall_reason`, and `wave_count`.

## Host-Trap Architecture

Host-trap sampling on gfx1151 follows the same CWSR daisy-chain model used on other supported generations:

```text
kernel thread -> SQ_CMD trap (BROADCAST + CHECK_VMID)
  -> CWSR first-level handler receives trap
  -> CWSR identifies HOST_TRAP and jumps to ROCr secondary handler
  -> ROCr handler writes a 64-byte sample into device_data
  -> buf_written_valX is incremented
  -> at watermark, ROCr signals the host thread
  -> ROCProfiler/consumer drains the buffer
```

## Implementation Notes

- The ROCr second-level handler must use `ttmp14:15` as the secondary TMA passed by the CWSR handler.
- Do not re-read TMA from hardware with `MSG_RTN_GET_TMA` in the ROCr second-level handler. Under CWSR, that returns the first-level CWSR TMA, not ROCr's secondary TMA.
- Using the wrong TMA was the root cause of earlier host-trap bugs where the handler dereferenced the wrong memory and produced bad sample state.
- The host-trap sample's HW ID field must be written from `HW_REG_HW_ID1`, matching the gfx1151 parser's HW-ID layout assumptions. Do not restore the previous raw `hwreg(24)` read: on gfx11.x it disassembles as `HW_REG_HW_ID2`.
- `GpuAgent::PcSamplingStart()` resets each XCC's host offsets, done signals, active buffer, and device-side `buf_write_val`/`buf_written_val0`/`buf_written_val1` counters. The device counters are cleared with `DmaFill`, which preserves restart behavior on large-BAR and non-large-BAR systems and prevents stale parity from making the host wait on the wrong written counter.

## Shared GFX11 Handler

The host-trap and stochastic paths share the same corrected gfx11 ROCr handler shape.

Relevant register usage in `projects/rocr-runtime/runtime/hsa-runtime/core/runtime/trap_handler/trap_handler.s`:
- `ttmp13` carries trap-type state for the gfx11 PC sampling path.
- gfx11 temporarily stores `buffer_id` in `ttmp6[31]`.
- `ttmp6[24:0]` carries the dispatch correlation bits.
- `ttmp14:15` holds the secondary TMA VA passed from the CWSR handler.
- `HW_REG_HW_ID1` is the source for the 32-bit HW-ID sample field in both gfx1151 host-trap and stochastic fill paths.

The older statement "`ttmp13 = buffer_id`" is stale and should not be restored. The old raw `hwreg(24)` / `HW_REG_HW_ID2` wording is also stale because raw register 24 maps to `HW_REG_HW_ID2`, not the HW-ID layout decoded by the parser.

Current validation scope is gfx1151. Some ROCr trap-handler conditionals are still written as generic gfx11 source conditionals, so non-gfx1151 gfx11 targets need validation or minor-version gating before claiming broader gfx11 support.

## ROCProfiler SDK and rocpd Updates

- `projects/rocprofiler-sdk/cmake/Modules/rocprofiler-sdk-utilities.cmake` narrows PC-sampling enablement from broad `gfx11xx` matching to implemented `gfx1151` and enables the gfx1151 stochastic test gating.
- `projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/pc_sampling/service.cpp` guards the final HSA stop path with `service->enabled` CAS so shutdown does not double-stop a PC-sampling service.
- `projects/rocprofiler-sdk/source/share/rocprofiler-sdk-rocpd/versions.yml` publishes the PC-sampling table as schema `3.0.4` while preserving upstream's SPM schema `3.0.3` as a compatibility snapshot.
- `projects/rocprofiler-sdk/source/lib/output/generateRocpd.cpp` requests `rocpd_version_triplet_t{3, 0, 4}` before inserting PC-sampling rows.
- `rocpd_pc_sampling.method` is required and checked to be either `host_trap` or `stochastic`.
- `projects/rocprofiler-sdk/source/lib/output/generateRocpd.cpp` writes `method='host_trap'` for host-trap samples and `method='stochastic'` for stochastic samples.
- `projects/rocprofiler-sdk/source/lib/output/generateRocpd.cpp` also writes stochastic-only `wave_issued_instruction`, `instruction_type`, `stall_reason`, and `wave_count` columns, matching stochastic CSV output.
- `projects/rocprofiler-sdk/source/lib/python/rocpd/README.md` documents `rocpd_pc_sampling`, the method column, and stochastic-only columns.

## Relevant Files

Kernel-space design references:
- `~/amdgpu-mainline/drivers/gpu/drm/amd/amdkfd/kfd_pc_sampling.c`
- `~/amdgpu-mainline/drivers/gpu/drm/amd/amdkfd/kfd_process.c`
- `~/amdgpu-mainline/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v11.c`

ROCr runtime and tests:
- `projects/rocr-runtime/runtime/hsa-runtime/core/runtime/trap_handler/trap_handler.s`
- `projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp`
- `projects/rocr-runtime/runtime/hsa-runtime/core/runtime/hsa.cpp`
- `projects/rocr-runtime/rocrtst/suites/functional/pc_sampling.{cc,h}`
- `projects/rocr-runtime/libhsakmt/tests/kfdtest/src/KFDPCSamplingTest.cpp`

ROCProfiler parser/service/tooling:
- `projects/rocprofiler-sdk/cmake/Modules/rocprofiler-sdk-utilities.cmake`
- `projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/pc_sampling/parser/translation.hpp`
- `projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/pc_sampling/parser/pc_record_interface.cpp`
- `projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/pc_sampling/parser/correlation.hpp`
- `projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/pc_sampling/parser/tests/gfx1151test.cpp`
- `projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/pc_sampling/code_object.hpp`
- `projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/pc_sampling/code_object.cpp`
- `projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/pc_sampling/service.cpp`

rocpd output:
- `projects/rocprofiler-sdk/source/lib/output/generateRocpd.cpp`
- `projects/rocprofiler-sdk/source/share/rocprofiler-sdk-rocpd/rocpd_tables.sql`
- `projects/rocprofiler-sdk/source/share/rocprofiler-sdk-rocpd/rocpd_views.sql`
- `projects/rocprofiler-sdk/source/lib/python/rocpd/README.md`

## Validation

```sh
./build_rocr.sh

HSA_DISABLE_XDNA=1 \
ROCRTST_PLATFORM_CONFIG="$PWD/projects/rocr-runtime/rocrtst/config/platform_config.yaml" \
./build/rocr-runtime-rocrtst/rocrtst64 --gtest_filter=rocrtstFunc.PC_Sampling_Extension_Config_Test:rocrtstFunc.PC_Sampling_Lifecycle_Test

HSA_DISABLE_XDNA=1 \
./build/rocr-runtime-kfdtest/kfdtest --gtest_filter=KFDPCSamplingTest.*

./build_rocprofiler.sh

./build_pc_sampling_test.sh

ctest --test-dir build/rocprofiler-sdk --output-on-failure -R "unit\.pcs_parser"

ctest --test-dir build/rocprofiler-sdk -j1 --output-on-failure -R "tests\.integration\.(execute\.rocpd-help|execute\.rocprofv3-test-rocpd$|validate\.rocprofv3-test-rocpd$)"

HSA_DISABLE_XDNA=1 python3 test_pc_sampling.py
```

Observed final results:
- ROCr lifecycle/config tests passed `2/2`.
- KFD PC-sampling validation passed all `5/5` enabled cases, including multi-thread and multi-process coverage.
- `unit.pcs_parser` passed `16/16`; focused PC-sampling service tests passed `11/11`.
- The rocpd integration group passed all 10 enabled tests; three multiprocess converter cases remained CTest-disabled.
- `HSA_DISABLE_XDNA=1 python3 test_pc_sampling.py` passed with `1,349` CSV lines in the latest installed-runtime run.
- Live RocPD validation produced schema `3.0.4` output with 177 `host_trap` PC-sampling rows. SQLite validation confirmed the method constraint, NULL stochastic-only columns for every host-trap row, `PRAGMA integrity_check = ok`, and zero foreign-key violations.
- The rebuilt installed runtime used for these checks reported version `7.16.26326-9095425e29`.
- These final runs were performed after rebuilding the `HW_REG_HW_ID1` trap-handler cleanup.
- The SDK service-configuration boundary test handles unbounded host-trap maxima without overflow and avoids session creation when KFD accepts values above its reported stochastic maximum.
`HSA_DISABLE_XDNA=1` is required on this machine for GPU-only validation because the local XDNA/NPU stack fails discovery/open. This skips the unhealthy NPU path without changing GPU PC-sampling behavior.

PC sampling tests that use runtime/KFD PC sampling resources should be run sequentially. Running ROCr lifecycle tests concurrently with KFD PC sampling tests can produce expected resource contention because both use the same KFD PC sampling facility.

Broad `rocprofv3-test-pc-sampling` integration validators are not the gfx1151 acceptance gate yet: several assert stale or unrelated assumptions such as every skid-prone sample having a non-zero dispatch/correlation id, older source-line ranges, and stall-reason allowlists. The standalone SDK sample consumer already ignores `correlation_id.internal == ROCPROFILER_CORRELATION_ID_INTERNAL_NONE`. Treat those samples as unmapped rather than as valid dispatch-join rows when writing future validators.

## Remaining Issues

- ROCr trap-handler changes for gfx1151 live inside gfx11 source conditionals. Confirm `TRAPSTS.HOST_TRAP` handling and the PC-sampling handler path on non-gfx1151 gfx11 targets, or narrow the assembly conditionals to gfx11.5 before advertising generic gfx11 behavior.
- ROCr PC-sampling TMA/device-data allocation and residency changes apply globally, not only to gfx1151. Validate dGPU and non-large-BAR paths, and make failure cleanup symmetrically call `MakeMemoryUnresident` after any successful residency mapping.
- `UpdateTrapHandlerWithPCS()` currently receives only the buffer pointer for the method being created. If host-trap and stochastic sessions must be mutually exclusive, enforce that globally; otherwise preserve the other active method's TMA pointer when updating the handler.
- Attach-mode PC-sampling code-object tracking registers existing/new code objects but does not remove stale ranges or decoders on unload. This matters for attach mode, long-running apps, and VA reuse.
- `rocpd_pc_sampling` lacks indexes for likely query keys such as `timestamp`, `dispatch_id`, and `correlation_id`. Large PC-sampling databases may query slowly.
- Broader rocpd multi-process/multi-agent semantics need additional columns or relations such as `nid`, `pid`, `tid`, agent identity, foreign keys, and reliable joins to dispatch records instead of relying only on raw `dispatch_id` and `correlation_id` values.
- rocpd PC-sampling output stores the stochastic-only fields already exposed by CSV. Lower-level stochastic metadata that CSV does not expose, such as sampling-lock error and arbiter issue/stall state, is only available through JSON/raw records.
