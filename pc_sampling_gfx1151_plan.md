# PC Sampling GFX1151 (Strix Halo) - Working Notes

## Goal
- Host-trap PC sampling on gfx1151 must produce non-empty `*pc_sampling_host_trap*.csv` without GPU fault/reset/hang.

## Baselines (user-provided stable checkpoints)
- `~/amdgpu`: `c4605eba04bf` (no page fault state).
- `~/rocm-systems`: `f9531de6d9` (no page fault state).
- WIP commits are mutable; use non-WIP commits for future bisect baselines.

## Current Loaded Stack (after current reboot)
- Kernel module path:
  - `/lib/modules/6.18.0-9-generic/updates/dkms/amdgpu.ko.zst`
- Loaded module srcversion:
  - `9CDD239679062C960F49B8E`
- Loaded module policy support (`modinfo -p`):
  - includes policies `12=single_wave_no_queue_no_vmid`, `13=broadcast_queue_wave_no_vmid_probe`
- Runtime knobs visible:
  - `/sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_sqcmd_policy`
  - `/sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_max_injected_traps`
  - `/sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_post_status_delay_us`

## Key Proven Facts
- Page-fault regression from 20260206 was fixed by restoring gfx11 `MSG_RTN_GET_TMA` decode (`<<8` + sign extension) in userspace trap code.
- Trap handler still does not enter for hosttrap path in current matrix:
  - `tma[1]` never reaches `0xDEAD`
  - `reserved0` stays `0x0`
  - `written0/written1` stay `0`
  - no `pc_sampling_host_trap.csv`
- Queue remap and trap programming are active in kernel logs:
  - `TRAP_EN=1`
  - TBA/TMA reprogrammed after remap
  - `pcs hosttrap: set target vmid=...`

## Experiment Summary (important runs)

### Page-fault isolation
- `run_tag=20260206_234009`
  - nonzero `post_status` then page fault / reset.
- `run_tag=20260206_234257`
  - after userspace TMA decode fix, no page fault/reset; still no CSV.

### Policy matrix before latest reboot (`srcversion=7DA1D81FC2330F9D89F8159`)
- `run_tag=20260207_010719`, `policy=4` (`broadcast_probe`, cap=8)
  - only run with clear pending-count signal: `post_status=0x1b` at `se=0 sh=0`.
  - still no handler-entry marker and no CSV.
- `run_tag=20260207_011136`, `policy=6` (`broadcast_queue_probe`)
  - all `post_status=0x0`; no CSV.
- `run_tag=20260207_011311`, `policy=7` (`single_fixed_q0_w0`)
  - all `post_status=0x0`; no CSV.
- `run_tag=20260207_011453`, `policy=8` (`single_queue_sweep_w0`)
  - all `post_status=0x0`; no CSV.
- `run_tag=20260207_011608`, `policy=9` (`noop`)
  - no SQ_CMD writes by design; symptom unchanged; no CSV.

### Previous reboot (`srcversion=BE5C663734DEE8F6D6E61C1`)
- `run_tag=20260207_012637`, `policy=1`
  - baseline unchanged; no markers, no CSV.
- `run_tag=20260207_013103`, `policy=11` (`broadcast_queue_no_vmid_probe`, cap=8)
  - `mode=2` and `check_vmid=0` confirmed in dmesg.
  - queue sweep `queue_id=0..7` executed.
  - all `post_status=0x0` with `se=-1 sh=-1`.
  - no `probe latched`, no marker movement, no CSV.

### Current reboot (`srcversion=9CDD239679062C960F49B8E`)
- `run_tag=20260207_014600`, `policy=1` (default), timed run with `PCS_TIMEOUT_SEC=15`
  - userspace symptom unchanged at timeout:
    - `tma[1]=0x0`
    - `reserved0=0x0`, `written0=0`, `written1=0`
    - no CSV
  - kernel dmesg confirms:
    - policy `single_queue_sweep(1)`
    - all observed `post_status=0x0` with `se=-1 sh=-1`
    - injection cap reached at 512, but loop kept running far beyond cap
    - later `MES failed to respond to msg=SUSPEND`
- `run_tag=20260207_015227`, `policy=12` (`single_wave_no_queue_no_vmid`, cap=64, delay=0)
  - userspace still shows no handler-entry markers and no CSV:
    - `tma[1]=0x0`
    - `reserved0=0x0`, `written0=0`, `written1=0`
  - kernel dmesg confirms:
    - `mode=0`, `check_vmid=0`, `queue_id=0`, wave sweep active
    - intermittent nonzero pending status appears (`post_status=0x28` / `0x21`, `se=0 sh=0`)
    - later waves return `post_status=0x0` again
    - injection cap reached at 64, then trigger loop continued
    - teardown hit `MES failed to respond to msg=SUSPEND` and forced `GPU reset begin`
- `run_tag=20260207_015800`, `policy=13` (`broadcast_queue_wave_no_vmid_probe`, cap=8, delay=0)
  - run completed quickly (no timeout/hang), but userspace still shows no handler-entry markers and no CSV:
    - `tma[1]=0x0`
    - `reserved0=0x0`, `written0=0`, `written1=0`
  - kernel dmesg confirms:
    - `mode=2` (`broadcast_queue`), `check_vmid=0`, with wave+queue encoded
    - first SQ_CMD gets nonzero pending status (`post_status=0x28`, `se=0 sh=0`)
    - probe latch fired immediately:
      - `probe latched post_status=0x28; disabling further SQ_CMD for vmid=10`
  - despite confirmed pending status and trap programming, no userspace handler entry/csv occurred.

## Conclusions (from completed experiments, not speculation)
1. The current blocker is not userspace buffer mapping anymore; it is missing trap delivery into handler path.
2. `CHECK_VMID=1` is not the primary blocker for queue-targeted mode:
   - `policy=11` (`broadcast_queue` + `CHECK_VMID=0`) still showed zero pending status and zero handler entry.
3. Nonzero `post_status` is reproducible in at least two policies (`policy=4` and now `policy=12`), but it still does not map to per-process handler entry.
4. `noop` reproducing the same userspace symptom confirms CSV failure is due to no delivered hosttrap, not due to post-processing.
5. Some long-running variants can wedge/timeout during teardown; short capped runs are the right iteration method.
6. `policy=12` increases confidence that SQ trap request can set pending status without reaching the userspace hosttrap handler.
7. The newest loaded module (`srcversion=9CDD...`) still has no successful handler delivery / CSV under tested policies (`1`, `12`).
8. `policy=10` has the same SQ_CMD encoding dimensions as `policy=11` (`broadcast_queue`, `check_vmid=0`, queue sweep). The only behavioral difference is probe mechanics (`policy=11` stops on first nonzero status and clamps cap), so `policy=10` is lower priority unless we intentionally need higher injection depth.
9. `policy=13` confirms the same core gap under a different trigger shape: pending status can be observed and latched, but hosttrap handler markers in userspace never move.
10. `policy=13` showed a transient status transition (`post_status 0x28 -> 0x0`) while userspace markers stayed zero. This is consistent with `CHECK_VMID=0` traps being consumed outside the target process/queue.

## New Kernel Prep Added (now loaded in current reboot)
- File: `drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v11.c`
- Added targeted policies (to isolate `QUEUE_ID` vs `WAVE_ID` effects):
  - `12=single_wave_no_queue_no_vmid`
    - `MODE_SINGLE`, `CHECK_VMID=0`, wave sweep, no queue targeting.
  - `13=broadcast_queue_wave_no_vmid_probe`
    - `MODE_BROADCAST_QUEUE`, `CHECK_VMID=0`, queue+wave sweep, probe behavior.
- Compile check passed:
  - `make -j32 M=drivers/gpu/drm/amd/amdgpu amdgpu_amdkfd_gfx_v11.o`

## Next Focused Plan (before/after next reboot)
1. Rebuild+install DKMS from current `~/amdgpu`, reboot, verify `srcversion` changed.
2. Keep short runs and test only the remaining new policy:
   - policy `13` (`broadcast_queue_wave_no_vmid_probe`) with low cap first (8 or 16) to reduce reset risk.
3. Optional tie-breaker:
   - run `policy=10` with low cap (e.g., 8/16) to confirm no hidden difference at shallow depth before trying any higher-risk caps.
4. Next targeted policy to avoid `queue_id` dependency while still targeting VMID:
   - `policy=14` (`single_wave_no_queue`, `MODE_SINGLE + CHECK_VMID=1 + wave sweep + no queue_id`).
   - optional `policy=15` probe variant (same shape, low-cap auto-disable).
5. If policy 14/15 still have no markers, stop SQ_CMD policy exploration and instrument trap-delivery path:
   - add kernel logs around hosttrap event decode/dispatch (trap-id check, vmid/queue/wave attribution, and userspace buffer write gate).
   - verify whether pending status corresponds to hosttrap event type expected by current userspace trap handler.

## New Kernel Update Prepared (needs DKMS install + reboot)
- File: `~/amdgpu/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v11.c`
- Added policies:
  - `14=single_wave_no_queue`:
    - `MODE_SINGLE`, `CHECK_VMID=1`, wave sweep, no `QUEUE_ID` field.
  - `15=single_wave_no_queue_probe`:
    - same as 14 with low-cap probe behavior.
- Build check:
  - `make -j32 M=drivers/gpu/drm/amd/amdgpu amdgpu_amdkfd_gfx_v11.o` passed.
3. For each run, compare only these signals:
   - userspace markers: `tma[1]`, `reserved0`, `written0/1`
   - kernel markers: `SQ_CMD` decode, `post_status`, `se/sh`, `probe latched`
4. Decision gate:
   - if either policy causes marker movement (`tma[1]=0xDEAD` or `reserved0=0xA1150002`), keep that policy and re-enable fuller sample-write path.
   - if both remain zero, next debug should move to trap-handler enable path semantics (not more SQ_CMD permutations).

## Runtime Defaults for Fast Iteration
- `PCS_INTERVAL_US=5000`
- `PCS_TIMEOUT_SEC=15`
- `PCS_TIMEOUT_KILL_AFTER_SEC=3`
- Logs: `pc_sampling_logs/<run_tag>/`
