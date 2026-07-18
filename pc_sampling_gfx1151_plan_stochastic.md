# Stochastic PC Sampling on gfx1151 (Strix Halo)

## Current Status

- ROCr captures real stochastic metadata on gfx1151.
- ROCProfiler decodes gfx1151 stochastic samples into non-default public fields.
- The SDK parser follows the local KFD IOCTL v1.5 / gc12-style stochastic snapshot layout for GC 11.5.
- End-to-end test passes with `Wave_Count` treated as unavailable/all-zero when ROCr cannot read `SQ_PERF_SNAPSHOT_DATA1`.
- Stochastic branch outcome fidelity is preserved for conditional branch samples.
- Decoded-instruction post-processing caches per-thread classification/effect data by `(code_object_id, code_object_offset)` so repeated samples avoid the synchronized code-object decoder hot path.
- The gfx1151 ROCr stochastic fill path captures HW ID with named `HW_REG_HW_ID1`, matching the parser's HW-ID decode, instead of raw `hwreg(24)`, which disassembles as `HW_REG_HW_ID2` on gfx11.x.

## ROCr Runtime

Implemented in `projects/rocr-runtime/runtime/hsa-runtime/core/runtime/trap_handler/trap_handler.s`:
- Preserves the stochastic/hosttrap discriminator instead of overwriting it with `buffer_id` before fill-path selection.
- Preserves `ttmp6[24:0]` across the shared watermark/done-signal phases. SPI initializes those dispatch-correlation bits when the wave is launched, so using `ttmp6` as scratch made every stochastic trap after the first one of a wave report a stale correlation. The shared phases use `ttmp13` once the fill path is selected, which is the gfx11 equivalent of upstream's `TTMP_REG1` scratch-register rule.
- Preserves the saved `v2:v3` backup during stochastic fill so trap return does not corrupt VGPR state.
- Reads `HW_REG_PERF_SNAPSHOT_DATA` before `PC_HI` and writes the real snapshot payload into the sample record.
- Leaves `perf_snapshot_data1`/`perf_snapshot_data2` zero-filled on gfx1151 because the gfx1151 assembler rejects `HW_REG_PERF_SNAPSHOT_DATA1` and `HW_REG_PERF_SNAPSHOT_DATA2` as unsupported hardware registers.
- Uses `HW_REG_HW_ID1` for the stochastic sample HW-ID field. Do not restore the raw `hwreg(24)` read because it maps to `HW_REG_HW_ID2` on gfx11.x.
- Keeps the working gfx1151 stochastic fill path without temporary debug instrumentation.

These fixes resolved the two real runtime failures observed during bring-up:
- Stochastic samples were written with the wrong record shape because the trap-type bit was lost before the fill-path branch.
- The first true stochastic fill path corrupted restored VGPR state and caused immediate gfxhub permission faults.

## ROCProfiler SDK

Relevant files:
- `projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/pc_sampling/parser/translation.hpp`
- `projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/pc_sampling/parser/pc_record_interface.cpp`
- `projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/pc_sampling/parser/correlation.hpp`
- `projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/pc_sampling/parser/tests/gfx1151test.cpp`
- `projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/pc_sampling/code_object.hpp`
- `projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/pc_sampling/code_object.cpp`
- `projects/rocprofiler-sdk/cmake/Modules/rocprofiler-sdk-utilities.cmake`
- `projects/rocprofiler-sdk/tests/rocprofv3/pc-sampling/stochastic/**/validate.py`

Implemented behavior:
- Added a dedicated `GFX1151` parser type so support is explicit and does not overclaim generic gfx11 support.
- Narrowed PC-sampling CMake enablement from broad `gfx11xx` matching to implemented `gfx1151` support.
- Wired gfx115 stochastic integration validators.
- Decodes gfx1151 stochastic metadata using the v1.5/gc12-style fields confirmed from local KFD/source evidence.
- Allows gfx115 CSV `Wave_Count == 0` because ROCr emits zero when `SQ_PERF_SNAPSHOT_DATA1` is unavailable from the trap handler.
- Keeps the generic `gfx11` stochastic parser path unchanged.

## gfx1151 Snapshot Layout

The local amdgpu/KFD source exposes PC-sampling IOCTL version `1.5` for GC 11.5 stochastic sampling. The relevant field definitions align with `projects/aqlprofile/linux/registers/gc/gc_12_1_0_sh_mask.h` rather than the older assumed gfx11 layout.

gfx1151 decode:
- `SQ_PERF_SNAPSHOT_DATA[0]`: valid bit.
- `SQ_PERF_SNAPSHOT_DATA[1]`: `Wave_Issued_Instruction`.
- `SQ_PERF_SNAPSHOT_DATA[5:2]`: raw hardware `INST_TYPE`.
- `SQ_PERF_SNAPSHOT_DATA[8:6]`: `NO_ISSUE_REASON` / stall reason.
- `SQ_PERF_SNAPSHOT_DATA[14]`: `SAMPLING_LOCK_ERR`.
- `SQ_PERF_SNAPSHOT_DATA1[5:0]`: `Wave_Count`.
- `SQ_PERF_SNAPSHOT_DATA1[16:9]`: arbiter issue-state bits.
- `SQ_PERF_SNAPSHOT_DATA1[24:17]`: arbiter stall-state bits.
- `SQ_PERF_SNAPSHOT_DATA2`: unused by the SDK gfx1151 parser.

This supersedes the stale assumption that gfx1151 `Wave_Count` came from `perf_snapshot_data[10:5]` and that `perf_snapshot_data1` was unused.

gfx1151 snapshot availability: the RDNA3.5 ISA `OPR_HWREG` id table defines only `perf_snapshot_pc_lo`/`perf_snapshot_pc_hi` (18/19) and `perf_snapshot_data` (27) for this generation. `perf_snapshot_data1`/`_data2` do not exist, and id 28 is `SQ_WAVE_IB_STS2`. The wave count (`WAVE_CNT`) and the arbiter issue/stall state are fields of `DATA1` on gfx12 only, so on gfx11.5 they are absent in hardware rather than merely unnamed: the trap handler emits zero for `perf_snapshot_data1`/`data2`, and end-to-end validators must treat gfx1151 `Wave_Count == 0` as unavailable rather than invalid. Do not probe `hwreg(28)`: it is `SQ_WAVE_IB_STS2`, whose value is constant for a given kernel and is not a snapshot. If wave-count-like context is needed, derive it from sampling statistics such as the distinct `HW_ID` wave slots observed per SIMD, and take the pipe for an issued or `ARBITER_WIN_EX_STALL` sample from the decoded instruction instead of the arbiter bits.

## Instruction Post-Processing

The parser first decodes raw hardware `INST_TYPE` from `SQ_PERF_SNAPSHOT_DATA[5:2]`, then uses resolved instruction text to improve public classification where safe.

Important behavior to preserve:
- `s_waitcnt` post-processing marks the sample as not issued and reports `WAITCNT` as the reason.
- Issued samples are classified from decoded mnemonics for families such as `VALU`, `MATRIX`, `SCALAR`, `TEX`, `LDS`, `FLAT`, `EXPORT`, `MESSAGE`, `BARRIER`, `BRANCH_TAKEN`, `JUMP`, and fallback `OTHER`.
- Conditional branch samples decoded as `s_cbranch*` preserve the raw hardware branch outcome. A raw `BRANCH_NOT_TAKEN` sample must not be overwritten to `BRANCH_TAKEN` solely because the decoded mnemonic is a conditional branch.
- The per-thread decoded-instruction cache stores classification/effect metadata by `(code_object_id, code_object_offset)`. This avoids repeatedly entering the single synchronized code-object decoder mutex for duplicate high-volume stochastic samples.

For stalled samples, the parser reports `Instruction_Type = NO_INST` unless an `s_waitcnt` post-processing rule applies.

## Validation

ROCr tests validate:
- The public PC sampling extension table is populated, including `hsa_ven_amd_pcs_iterate_configuration`.
- gfx1151 exposes hosttrap and stochastic PC sampling configurations.
- Basic create/start/stop/flush/destroy/restart lifecycle works through the HSA extension path.
- KFD reports both gfx1151 hosttrap and stochastic capability entries.

ROCProfiler unit tests in `projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/pc_sampling/parser/tests/gfx1151test.cpp` validate:
- gfx1151 parser selection instead of generic gfx11 overclaiming.
- gfx1151 HW ID decode for stochastic and host-trap record shapes.
- valid/issued metadata extraction.
- separation of `wave_count` from `reason_not_issued`.
- `sampling_lock_error` decode.
- arbiter issue/stall state decode.
- raw hardware instruction type decode.
- mnemonic-derived instruction classification.
- stalled-sample behavior with `Instruction_Type = NO_INST`.
- conditional branch outcome preservation.

The relevant CTest tests are:
- `unit.pcs_parser.gfx1151_parser_test`
- `unit.pcs_parser.gfx1151_instruction_classification`
- `unit.pcs_parser.gfx1151_decoded_instruction_post_process`

Validation passed:

```sh
./build_rocprofiler.sh

./build_pc_sampling_test.sh

ctest --test-dir build/rocprofiler-sdk --output-on-failure -R "unit\.pcs_parser"

HSA_DISABLE_XDNA=1 python3 test_stochastic_sampling.py
```

Observed final results:
- `unit.pcs_parser` passed `16/16`. The SDK PC-sampling groups (`unit.pc_sampling`, `unit.pc_sampling_queue_hooks`, `unit.pc_sampling_versioning`) passed `31/31`.
- ROCr lifecycle/config tests passed `2/2`, and KFD PC-sampling validation passed all `5/5` enabled cases.
- `HSA_DISABLE_XDNA=1 python3 test_stochastic_sampling.py` passes with roughly `24.7k-24.9k` decoded rows and `2.5k` issued rows per run, populated instruction/reason metadata, and expected all-zero gfx1151 `Wave_Count` where the hardware register is unavailable.
- The current live RocPD schema check produced `1,238` `host_trap` rows in schema `3.0.4`. It validated the shared PC-sampling table and host-trap nullability but did not generate stochastic database rows in this run.
- SQLite validation confirmed the method constraint, NULL stochastic-only columns for host-trap rows, `PRAGMA integrity_check = ok`, and zero foreign-key violations.
- The rebuilt installed runtime used for these checks reports rocprofiler-sdk `1.4.2` with HIP `7.17.26396` and ROCr `1.21.0`, installed on top of `upstream/develop` `f69d3320ce`.
- These final runs were performed after rebuilding the `HW_REG_HW_ID1` trap-handler cleanup.
- The SDK service-configuration boundary test uses the largest `uint64_t` interval for finite-bound invalid input because the current KFD accepts values above its reported stochastic maximum. Host-trap time may report an unbounded `UINT64_MAX` maximum.
The end-to-end test requires decoded stochastic rows with non-default `Instruction_Type` and `Stall_Reason` data and no new amdgpu page-fault markers. On gfx1151, `Wave_Count` can be all zero because `SQ_PERF_SNAPSHOT_DATA1` is not readable from the ROCr trap handler.

The stochastic side of the `rocprofv3-test-pc-sampling` family passes together with the rest of the PC-sampling group (`65/65` enabled cases). The stochastic validators needed gfx1151-aware expectations rather than allowlist widening alone: `WAITCNT`, `SLEEP_WAIT` and `ARBITER_WIN_EX_STALL` are valid gfx1151 stall reasons, issued `s_nop` samples are `SCALAR`, and `Wave_Count` plus the arbiter issue/stall state are zero because `SQ_PERF_SNAPSHOT_DATA1` is unavailable, so the arbiter assertions are skipped for agents where that register cannot be read.

`HSA_DISABLE_XDNA=1` is required on this machine for GPU-only validation because the local XDNA/NPU stack fails discovery/open. This skips the unhealthy NPU path without changing GPU stochastic PC-sampling behavior.

## Remaining Issues

- ROCr stochastic support currently shares gfx11 trap-handler source conditionals with gfx1151-specific behavior. Confirm the host-trap/perf-snapshot conditionals on non-gfx1151 gfx11 targets, or narrow the assembly conditionals to gfx11.5 before advertising broader gfx11 stochastic support.
- ROCr PC-sampling TMA/device-data allocation and residency changes apply globally, not only to gfx1151. Validate dGPU and non-large-BAR paths, and make failure cleanup symmetrically call `MakeMemoryUnresident` after any successful residency mapping.
- `UpdateTrapHandlerWithPCS()` currently receives only the buffer pointer for the method being created. If host-trap and stochastic sessions must be mutually exclusive, enforce that globally. Otherwise preserve the other active method's TMA pointer when updating the handler.
- Mixed-agent stochastic validators choose gfx115/gfx9/gfx12 rules for the whole test run based on any matching agent name. This is fine for the current one-GPU gfx1151 scope, but should become per-agent or per-sample before heterogeneous validation.
- The decoded-instruction cache has unit/build validation but no new high-volume performance benchmark after the cache landed.
- rocpd identifies stochastic rows with `method='stochastic'` and stores the stochastic fields that CSV exposes: `wave_issued_instruction`, `instruction_type`, `stall_reason`, and `wave_count`. Lower-level metadata not emitted by CSV, such as sampling-lock error and arbiter issue/stall state, remains JSON/raw-record-only unless a future analysis need requires DB columns or structured `extdata`.
- Broader attach-mode code-object unload handling is tracked in the general host-trap PC-sampling plan because stale ranges/decoders affect instruction resolution for both host-trap and stochastic samples.
