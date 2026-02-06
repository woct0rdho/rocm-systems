# PC Sampling GFX1151 (Strix Halo) - Working Notes

## Goal
- Host-trap PC sampling must produce a non-empty `*pc_sampling_host_trap*.csv` without GPU fault/reset/hang.

## Stable Baselines
- `~/amdgpu`: `c4605eba04bf` (user-reported no page-fault state).
- `~/rocm-systems`: `f9531de6d9` (user-reported no page-fault state).
- WIP commits are mutable; use explicit non-WIP baseline commits for checkpoints.

## Current Loaded Stack (2026-02-06 23:31)
- Kernel module:
  - `/lib/modules/6.18.0-9-generic/updates/dkms/amdgpu.ko.zst`
  - `srcversion=58E6B23A5896A86B59EF5CC`
- Kernel trigger policy currently loaded:
  - `CMD=TRAP`, `MODE=BROADCAST`, `CHECK_VMID=1`, `WAVE_ID=0`, no explicit `QUEUE_ID`, injection cap `4096`.
  - detailed logs for `SQ_CMD`, `status`, `post_status`.
- Kernel source prepared (not loaded yet):
  - `CMD=TRAP`, `MODE=SINGLE`, `CHECK_VMID=1`, explicit `wave_id` + `queue_id` sweep, injection cap `512`.
  - policy label in dmesg: `single_queue_sweep`.
- Userspace trap handler current test path:
  - gfx11 hosttrap path uses `GET_TMA`, then minimal VMEM marker path (`reserved0` marker write) with early return.
  - temporary isolation also routes `trap_id==0` to hosttrap path on gfx11.

## Key Historical Anchors
- `run_tag=20260206_211141`:
  - saw nonzero trap-status transitions (`0x0 -> 0xa`, `0x0 -> 0x23`), then TCP page faults and MES reset.
- `run_tag=20260206_232100` (full gfx11 trap body + current kernel policy):
  - hang/timeout; manual kill required.
  - one transient `post_status=0xe`, then mostly `0x0`; injection cap hit; later MES reset.

## New Experiment Log (after reboot with `58E6B...`)
- `run_tag=20260206_232711`:
  - userspace step-B save/restore-only path.
  - finished quickly; no CSV.
  - host buffer unchanged (`reserved0=0x0`, `written0=0`, `written1=0`).
  - dmesg: `SQ_CMD=0xc0000495`, `post_status=0x0` for counts 1..20.
- `run_tag=20260206_232845`:
  - enabled minimal VMEM marker path while still decoding `GET_TMA` with `<<8`.
  - finished quickly; no CSV; no marker movement.
- `run_tag=20260206_233013`:
  - fixed userspace decode: `GET_TMA` now treated as raw returned address (no `<<8`, no sign-extend).
  - finished quickly; no CSV; no marker movement.
  - dmesg: `SQ_CMD=0xe0000495`, `post_status=0x0` for counts 1..20.
- `run_tag=20260206_233154`:
  - extra isolation: forced gfx11 `trap_id==0` path into hosttrap handler path.
  - finished quickly; no CSV; no marker movement.

## New Findings
- ISA check (`rdna35_instruction_set_architecture.md`): `MSG_RTN_GET_TMA` returns TMA address directly (`[31:0]/[63:0]`), so previous `<<8` decode was incorrect.
- Correcting `GET_TMA` decode did **not** change behavior under current kernel trigger policy.
- Even with broad userspace routing (`trap_id==0` forced to hosttrap path), host marker did not move.
- Strong current inference: under `broadcast_no_queue` policy, trap injection is likely not reaching executable trap-handler path on gfx1151, despite `SQ_CMD` writes and `post_status=0x0`.

## Fast Iteration Defaults
- `PCS_INTERVAL_US=5000`
- `PCS_TIMEOUT_SEC=15`
- `PCS_TIMEOUT_KILL_AFTER_SEC=3`
- Per-run artifacts: `pc_sampling_logs/<run_tag>/`

## Next Plan
1. Rebuild/install DKMS from current `~/amdgpu` source (requires sudo), then reboot.
2. Keep userspace minimal marker path unchanged to maximize signal (did handler run or not).
3. Run `./test_pc_sampling.sh` and compare:
   - `post_status` transitions,
   - host marker (`reserved0` / `written*`),
   - CSV presence.
4. If still no handler entry, next kernel step is toggling `CHECK_VMID` and/or queue filtering policy (`MODE_SINGLE` vs `MODE_BROADCAST_QUEUE`) under low cap.
