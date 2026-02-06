# PC Sampling GFX1151 (Strix Halo) - Working Notes

## Goal
- Host-trap PC sampling must produce a non-empty `*pc_sampling_host_trap*.csv` without GPU fault/reset/hang.

## Stable References
- `~/amdgpu`: `c4605eba04bf` (user-reported no page fault state).
- `~/rocm-systems`: `f9531de6d9` (user-reported no page fault state).
- WIP commits are mutable; use explicit non-WIP baseline commits for checkpoints.

## Current State (2026-02-06 20:12)
- Loaded DKMS module:
  - path: `/lib/modules/6.18.0-9-generic/updates/dkms/amdgpu.ko.zst`
  - `modinfo` srcversion: `F513467DF2BEA20E40C678C`
- Kernel trigger policy currently loaded:
  - deterministic `cmd=5`, `mode=single`, `check_vmid=0`, rotating `wave_id`, sweeping `queue_id`.
- Kernel trigger policy prepared in source (not loaded yet):
  - `check_vmid=1` enforced,
  - broadcast-first (`mode=broadcast`) + queue sweep, then alternating broadcast/single,
  - status transition logs for `sq_hosttrap_status` and `post_status`.
- Userspace currently installed (`rocr` rebuilt/installed):
  - gfx11 hosttrap uses `S_SENDMSG_RTN GET_TMA`, then canonicalization (`<<8` + sign-extension).
  - removed direct debug store to `TMA[1]`.
  - preserves/restores `ttmp0/ttmp1` on gfx11 hosttrap path.

## Experiment Log (Condensed)
- `run_tag=20260206_200403`
  - Fault reproduced with address pattern indicating wrong TMA decode.
  - `tma_ptr=0x76c036600000`, but dmesg fault page `0x00000076c0366000` (exact `>>8` pattern).
  - `post_status=0x28` seen once; timeout/hang.
- Userspace fix applied:
  - decode `GET_TMA` with `<<8` + sign-ext.
  - remove direct debug write to `TMA[1]`.
- `run_tag=20260206_200813`
  - shifted-TMA fault signature disappeared.
  - new faults observed on hosttrap buffer pages (`0x7889a860xxxx`) with `Faulty UTCL2 client ID: SQC (inst)`.
  - timeout/hang still present.
- Userspace fix applied:
  - preserve/restore `ttmp0/ttmp1` (trap return PC) to avoid clobbering return path.
- `run_tag=20260206_201136`
  - vmid=10 trigger loop shows `post_status=0x0` for counts 1..11000 (no pending trap observed).
  - userspace flush header remains unchanged: `reserved0=0x0`, `written0=0`, `written1=0`.
  - no CSV produced; timeout path hit.
  - separate lingering vmid=8 faults (same `pc_sampling_test` pid) appear during run and cause GPU reset (`MES might be in unrecoverable state`).
- 2026-02-06 20:18 kernel patch prepared (not loaded):
  - updated `kgd_gfx_v11_trigger_pc_sample_trap()` to prevent cross-VMID triggering (`check_vmid=1`) and improve hit probability (broadcast-first queue sweep).
  - added status transition logging for faster isolation.

## Key Findings
- Confirmed: raw `GET_TMA` value must be canonicalized for gfx1151 trap path.
- Confirmed: gfx11 trap handler must preserve `ttmp0/ttmp1`; clobber can lead to instruction-fetch faults.
- Remaining primary issue:
  - host trap still not reliably observed for active run (`post_status` stays `0x0`, no host buffer counters change).
- Secondary issue affecting iteration quality:
  - timeout path can leave stale vmid queues/process state that later faults and triggers GPU reset, contaminating subsequent runs.

## Current Code State
- Userspace (`rocr`):
  - `projects/rocr-runtime/runtime/hsa-runtime/core/runtime/trap_handler/trap_handler.s`
  - includes `GET_TMA` canonicalization and `ttmp0/ttmp1` restore.
- Kernel source (loaded patchline):
  - `drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v11.c`
  - deterministic single-wave trigger policy; queue sweep; vmid-reset logging.
- Kernel source (new patch prepared, requires DKMS reinstall + reboot):
  - same file, now with `check_vmid=1`, broadcast-first policy, and status transition debug.

## Fast Iteration Defaults
- `test_pc_sampling.sh`
  - `PCS_INTERVAL_US=5000`
  - `PCS_TIMEOUT_SEC=15`
  - `PCS_TIMEOUT_KILL_AFTER_SEC=3`
- Per-run logs:
  - `pc_sampling_logs/<run_tag>/config.log`
  - `pc_sampling_logs/<run_tag>/rocprof_run.log`
  - `pc_sampling_logs/<run_tag>/dmesg_since_start.log`

## Next Plan
1. Create a proper non-WIP baseline commit for current userspace/kernel debug state.
2. Rebuild/install DKMS from latest `~/amdgpu` patch and reboot (sudo required by user).
3. Re-run `./test_pc_sampling.sh` and gate on:
   - any nonzero `post_status`,
   - host-buffer header movement (`reserved0/written0/written1`),
   - CSV creation.
4. If still no host-buffer movement but `post_status` toggles:
   - add one minimal trap-side marker write into `host_trap_buffers->reserved0` only (avoid TMA writes).
