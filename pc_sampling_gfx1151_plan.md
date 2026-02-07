# PC Sampling gfx1151 (Strix Halo) - Debug Plan

## Goal
- Host-trap PC sampling on `gfx1151` must produce non-empty `*pc_sampling_host_trap*.csv`.
- No GPU VM fault, no MES suspend/reset, no teardown hang after timeout.

## Guardrails
- Ask user to run all sudo-required operations (DKMS install/reboot/sysfs writes).
- Keep experiments short by default:
  - `PCS_TIMEOUT_SEC=15`
  - `PCS_TIMEOUT_KILL_AFTER_SEC=3`
- Use stable commits as baselines, not amendable WIP commits.

## Baselines
- `~/amdgpu`: `c4605eba04bf` (user-confirmed no page fault state)
- `~/rocm-systems`: `f9531de6d9` (user-confirmed no page fault state)

## Current Runtime Stack (latest confirmed)
- Kernel: `6.18.0-9-generic`
- Module: `/lib/modules/6.18.0-9-generic/updates/dkms/amdgpu.ko.zst`
- Loaded `srcversion`: `C2B6CC540C5239330B31416`
- Current knobs used in latest runs:
  - `amdkfd_gfx11_pcs_sqcmd_policy=14`
  - `amdkfd_gfx11_pcs_max_injected_traps=64`
  - `amdkfd_gfx11_pcs_post_status_delay_us=0`
  - `amdkfd_gfx11_pcs_wave_scan_on_reset=1`
  - `amdkfd_gfx11_pcs_wave_scan_max_waves=32`
  - `amdkfd_gfx11_pcs_wave_target_debug=1`
  - `amdkfd_gfx11_pcs_runtime_reg_readback_on_reset=1`

## High-Value Historical Runs
- `run_tag=20260207_110823` (policy 1):
  - active-wave hit + VM fault at address consistent with `TMA >> 8` decode mismatch.
  - established userspace TMA decode issue existed.
- `run_tag=20260207_114819` (policy 14):
  - trap programming/readback matched intended TBA/TMA byte addresses.
  - no handler marker (`tma[1]` stayed `0x0`), no CSV.
  - SQ_CMD issued, but `post_status` stayed `0`.
- `run_tag=20260207_121848` (policy 14, src `C2B6...`):
  - `status=0x1` observed repeatedly; SQ_CMD suppressed by pending-status gate.
  - broad wave-dump spam with sentinel reads; no samples.
- `run_tag=20260207_122339` (policy 14, src `C2B6...`):
  - clean start with vmid=9; trap programming/readback still correct.
  - SQ_CMD issued (`0x900x0485`) and `post_status=0` for all logged injections.
  - userspace still no handler entry marker (`tma[1]=0x0`), no CSV.
  - run hangs in timeout teardown; later MES suspend failure and GPU reset in dmesg.

## Consolidated Findings
1. Trap-handler programming is correct on runtime readback:
   - intended TBA/TMA byte and programmed register forms match readback.
2. Userspace handler is still never reached in current failing path:
   - `tma[1]` marker remains `0x0` (expected to change on handler entry).
3. SQ_CMD trigger path is unreliable in two distinct modes:
   - mode A: stale/nonzero hosttrap status causes SQ_CMD skipping;
   - mode B: SQ_CMD is written but `post_status` remains `0` (no pending host trap).
4. Policy sweeping alone has not produced a CSV across many runs; issue is deeper than one policy value.
5. `git diff dkms` review shows several debug patches are state-changing, not log-only:
   - `kfd_process_set_trap_handler` now programs TBA/TMA and calls `remap_queue`.
   - the labeled "readback after remap" path re-programs registers again (side effect).
   - MES `remap_queue` now removes/adds all queues (ignores filter granularity in MES path).
   - VMID allocation/deallocation timing changed (first queue alloc + last queue free).
6. Heavy wave/status debug paths can perturb timing/state visibility:
   - wide SE/SH/wave scans, repeated `dev_info`, and optional `udelay` in trigger loop.
   - this can alter trap cadence and queue remap timing while diagnosing.

## New Source Changes (not yet deployed)
### Delta reduced for isolation (`git diff dkms`)
- Only 3 files remain changed in `~/amdgpu`:
  - `drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v11.c`
  - `drivers/gpu/drm/amd/amdkfd/kfd_pc_sampling.c`
  - `drivers/gpu/drm/amd/amdkfd/kfd_priv.h`
- Reverted to `dkms` baseline:
  - `kfd_process.c` (removed remap/reprogram debug side effects)
  - `kfd_device_queue_manager.c` (removed MES remove/add-all remap path and VMID lifecycle experiment)
  - `kfd_packet_manager_v9.c` (removed extra map-process logs)
  - `kfd_debug.c` (removed test wrapper noise)

### Remaining active logic
- `kfd_pc_sampling`:
  - gfx11.5 capability entries added.
  - hosttrap trigger now targets explicit per-session VMID (`target_vmid`) instead of `last_vmid_kfd`.
- `amdgpu_amdkfd_gfx_v11`:
  - keeps gfx11 trap programming and SQ_CMD trigger path/policies.
  - runtime perturbation reduced:
    - `amdkfd_gfx11_pcs_wave_scan_on_reset` default set to `0`
    - `amdkfd_gfx11_pcs_wave_target_debug` default set to `0`
    - removed extra mid-run global-wave snapshot hook.

### Userspace reminder
- `~/rocm-systems/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/trap_handler/trap_handler.s`
  - keep gfx11 TMA decode fix and early host-trap routing.

## Next Plan (reboot-efficient)
1. Keep policy fixed (no new sweep) and run one short repro (`PCS_TIMEOUT_SEC=15 ./test_pc_sampling.sh`).
2. Prioritize isolation of state-changing debug code:
   - separate "log-only" vs "behavior-changing" patches in `kfd_process.c` and `kfd_device_queue_manager.c`.
   - keep `program_trap_handler_settings_v11` and SQ_CMD path, but avoid extra remap/reprogram loops.
3. Inspect only high-signal logs:
   - `global_wave_summary` / `global_wave ...`
   - `count=... status=... pending_count=...`
   - `SQ_CMD=...` and `post_status=...`
4. Branch based on evidence:
   - if `global_wave_summary` shows target VMID has no valid waves at trigger time, refine targeting strategy (policy/queue selection).
   - if target VMID has valid trap-enabled waves but `post_status` still stays `0`, isolate SQ_CMD acceptance path further (command semantics/register behavior).
   - if pending becomes nonzero but still no userspace marker, focus on trap-entry micro-path before first buffer write.

## Immediate Ask Before Next Reboot
- After each kernel-side behavior change, ask user to rebuild/reinstall DKMS and reboot (sudo path).
- Keep kernel boot/module params stable across A/B runs to preserve comparability.
