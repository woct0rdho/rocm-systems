# PC Sampling GFX1151 (Strix Halo) - Working Notes

## Goal
- Host-trap PC sampling must produce a non-empty `*pc_sampling_host_trap*.csv` without GPU fault/reset/hang.

## Stable Baselines
- `~/amdgpu`: `c4605eba04bf` (user-reported no page-fault state).
- `~/rocm-systems`: `f9531de6d9` (user-reported no page-fault state).
- WIP commits are mutable; use explicit non-WIP baselines for checkpoints.

## Current Loaded Stack (2026-02-06 23:50)
- Kernel module:
  - `/lib/modules/6.18.0-9-generic/updates/dkms/amdgpu.ko.zst`
  - `srcversion=FD57E0E85BEA8F5C7196FD3`
- Kernel trigger policy on gfx11:
  - `CMD=TRAP`, `MODE=SINGLE`, `CHECK_VMID=1`, `wave_id+queue_id` sweep.
  - dmesg policy label: `single_queue_sweep`.
- Userspace trap handler test path:
  - gfx11 hosttrap path uses `MSG_RTN_GET_TMA`, decode with `<<8` + sign extension.
  - minimal marker path writes `reserved0` marker (`0xA1150002`) then returns.
  - temporary isolation keeps gfx11 `trap_id==0` routed to hosttrap path.

## New Code Update (2026-02-06 23:56, not loaded yet)
- Kernel file changed:
  - `~/amdgpu/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v11.c`
- Added runtime module parameter for SQ_CMD policy selection:
  - `amdkfd_gfx11_pcs_sqcmd_policy` (sysfs path after reboot:
    `/sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_sqcmd_policy`)
  - Values:
    - `0`: `single_q0` (Variant A)
    - `1`: `single_queue_sweep` (current behavior)
    - `2`: `broadcast` (Variant B, gfx12-like)
    - `3`: `single_queue_sweep_no_vmid_check` (Variant C)
- Dmesg now logs policy name on reset and each `SQ_CMD` trace line.
- Local compile sanity check passed:
  - `make -j32 M=drivers/gpu/drm/amd/amdgpu amdgpu_amdkfd_gfx_v11.o`

## Key Experiments
- `run_tag=20260206_234009`:
  - first 10 injections: `post_status=0x0`.
  - at count 11 (`wave_id=10 queue_id=2`): gfxhub page faults, `post_status=0x28`, MES failures, GPU reset.
  - fault VA pattern matched a `tma_ptr >> 8` style mismatch.
- `run_tag=20260206_234257`:
  - after restoring userspace `GET_TMA` decode (`<<8` + sign-ext), no page faults/resets.
  - still no CSV; host buffer unchanged (`reserved0=0`, `written0=0`, `written1=0`).
  - dmesg `post_status=0x0` for counts 1..20.
- `run_tag=20260206_234359`:
  - repeat stable/no-fault run; same no-CSV, no-marker behavior.
  - dmesg `post_status=0x0` across trigger sweep.
- `run_tag=20260206_235047`:
  - same result under current stack (no CSV, marker unchanged).
  - dmesg still `post_status=0x0` with `single_queue_sweep`.

## Findings
- Restoring `GET_TMA` decode prevents the page-fault/reset regression from `234009`.
- Current blocker is now "no trap delivery" rather than crash:
  - host marker never changes,
  - `post_status` remains `0x0`,
  - CSV not produced.
- Strong current hypothesis: `SQ_CMD` targeting/filtering does not hit a live wave context on gfx1151 in current policy (`mode/check_vmid/queue_id/wave_id` combination).

## Fast Iteration Defaults
- `PCS_INTERVAL_US=5000`
- `PCS_TIMEOUT_SEC=15`
- `PCS_TIMEOUT_KILL_AFTER_SEC=3`
- Logs per run: `pc_sampling_logs/<run_tag>/`

## Next Plan
1. Rebuild/install DKMS from current `~/amdgpu` source and reboot (required to load new module param support).
2. Run short A/B/C isolation without further rebuilds by changing:
   - `/sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_sqcmd_policy`
3. For each policy (`0`, `2`, `3`), run `./test_pc_sampling.sh` and compare:
   - `post_status` transitions,
   - host marker (`reserved0`),
   - CSV creation.
4. If any policy gives nonzero marker/post_status, keep that policy and then restore fuller sample-write body.
