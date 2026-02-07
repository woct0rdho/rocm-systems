# PC Sampling gfx1151 (Strix Halo) - Current Debug Plan

## Objective
- Make host-trap PC sampling on `gfx1151` produce non-empty `*pc_sampling_host_trap*.csv`.
- Avoid regressions: no GPU VM fault, no MES reset, no timeout teardown hang.

## Working Rules
- User runs all sudo-required steps (DKMS install/reboot/sysfs writes).
- Keep experiments short unless explicitly needed:
  - `PCS_TIMEOUT_SEC=15`
  - `PCS_TIMEOUT_KILL_AFTER_SEC=3`
- Hold one kernel policy/config constant while isolating one variable.

## Rollback Anchors
- `~/amdgpu` baseline (user-confirmed no page fault): `c4605eba04bf`
- `~/rocm-systems` baseline (user-confirmed no page fault): `f9531de6d9`
- User-created safety commits:
  - `~/amdgpu`: `5f7e28722866503ac13b854b1e30bb401b7c4965`
  - `~/rocm-systems`: `206a34691f040397ddb1b3c0366880500a24dfe8`

## Current Active Deltas
### Kernel (`~/amdgpu`, vs `dkms`)
- Only 3 files intentionally active:
  - `drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v11.c`
  - `drivers/gpu/drm/amd/amdkfd/kfd_pc_sampling.c`
  - `drivers/gpu/drm/amd/amdkfd/kfd_priv.h`
- Key behavior kept:
  - gfx11.5 capability plumbed.
  - PC-sampling trigger targets session VMID.
  - SQ_CMD policy framework and focused logs.
- New debug (added, requires DKMS reinstall+reboot to activate):
  - `trigger_pc_sample_trap: tma_check ...`
  - logs intended TMA byte/reg and currently programmed TMA reg/byte at trigger time.

### Userspace (`~/rocm-systems`, vs `develop` in `projects/rocr-runtime`)
- `trap_handler.s`:
  - Kept gfx11 TMA decode fix (`GET_TMA << 8`, sign-extend).
  - Removed state-mutating debug writes (`TMA[1]`, `reserved0` markers).
  - Restored conservative `trap_id==0 -> .not_s_trap` path.
- `amd_gpu_agent.cpp`:
  - Broadened gfx1151 blob selection to `major=11 && minor=5` (no stepping hard-gate).
  - Kept high-signal TMA pointer/SetTrapHandler logs.
  - Added host-side `ptr_range` log in flush when `old_val > 0`.

## High-Value Experiment Log (kept)
| run_tag | policy | Key result | Why it matters |
|---|---:|---|---|
| `20260207_110823` | 1 | Active-wave hit + VM fault at address consistent with `TMA >> 8` mismatch | Established original userspace TMA decode bug |
| `20260207_114819` | 14 | Trap programming/readback correct, but no samples, `post_status=0` | Showed problem remained after basic setup |
| `20260207_121848` | 14 | `status=0x1`, SQ_CMD skipped by pending gate | Revealed “pending-sticky” failure mode |
| `20260207_122339` | 14 | SQ_CMD issued, `post_status=0`, no CSV; timeout path later led to MES suspend/reset | Revealed “SQ_CMD no-latch” mode + teardown fragility |
| `20260207_130703` | 14 | After userspace cleanup, still `old_val=0`, no CSV | Excluded removed userspace debug mutations as root cause |
| `20260207_131032` | 14 | One wave reported valid+trap-enabled, still `post_status=0` | Strong signal against pure wave-selection explanation |
| `20260207_131611` | 14 | Same failure on new VMID; `old_val=0`; `ptr_range` never prints | Confirms failure not tied to one VMID/process instance |

## Current Findings
1. Trap programming appears correct (TBA/TMA readback matched intended values in prior runs).
2. Userspace trap path is not reached (`old_val` remains `0`, no sample writes visible).
3. Two kernel trigger failure shapes exist:
   - pending bit nonzero -> SQ_CMD suppressed;
   - SQ_CMD issued -> `post_status=0` (no hosttrap pending).
4. Policy sweeps have low yield now; they have not produced CSV and mostly add noise.
5. Removed userspace debug state mutations did not change failure outcome.
6. Current primary blocker is before userspace sample decode/copy: hosttrap request is not becoming effective.

## Most Likely Fault Zone
- Kernel trigger-to-hosttrap delivery/acceptance path (SQ_CMD semantics or trap pipeline gating on gfx1151).
- Not currently pointing to CSV parser/host-buffer logic.

## Next Plan (minimum reboot cost)
1. Keep config fixed for comparability (`policy=14`, same timeout/settings).
2. Activate new kernel `tma_check` log (single DKMS reinstall + reboot).
3. Run one short repro and decide by evidence:
   - If `tma_check` mismatches intended TMA at trigger: fix trap programming path.
   - If `tma_check` matches and wave is valid+trap-enabled but `post_status=0`: isolate SQ_CMD acceptance/gating semantics (cmd/mode/data/check_vmid path).
   - If `post_status` becomes nonzero but `old_val` stays zero: instrument first trap-side sample destination pointer before first store in trap handler.
4. Stabilize timeout cleanup path (ensure stuck chain is force-killed) to reduce manual recovery overhead.

## Reboot Checklist (commands)
Run after reboot to set policy knobs consistently:

```bash
echo 14 | sudo tee /sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_sqcmd_policy
echo 64 | sudo tee /sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_max_injected_traps
echo 0 | sudo tee /sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_post_status_delay_us
echo 1 | sudo tee /sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_wave_scan_on_reset
echo 32 | sudo tee /sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_wave_scan_max_waves
echo 1 | sudo tee /sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_wave_target_debug
echo 1 | sudo tee /sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_runtime_reg_readback_on_reset
```

Short repro command:

```bash
PCS_TIMEOUT_SEC=15 ./test_pc_sampling.sh
```
