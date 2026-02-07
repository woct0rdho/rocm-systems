# PC Sampling gfx1151 (Strix Halo) - Compacted Debug Plan

## Goal
- Host-trap PC sampling on `gfx1151` must generate non-empty `*pc_sampling_host_trap*.csv` without VM fault/hang regressions.

## Current Status (most important)
- `target_vmid=0` root cause is fixed in latest direction:
  - `gfx_v11_kfd2kgd.get_atc_vmid_pasid_mapping_info` is now active.
  - runtime shows PASID->VMID resolve (`owner_pasid=32770 -> vmid=8`).
- New blocker:
  - trigger runs for `vmid=8`, but runtime trap regs for that VMID are still zero in trigger path (`expected_valid=0`, `TBA/TMA=0`).
  - SQ_CMD injected repeatedly, `post_status=0`, no hosttrap latch, no CSV.
- Conclusion:
  - VMID resolution is no longer the bottleneck.
  - trap-register programming/apply-on-target-VMID is the primary suspect.

## Active Code Deltas
### Kernel (`~/amdgpu`)
- `drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v11.c`
  - Added gfx11 `get_atc_vmid_pasid_mapping_info_v11()` (IH VMID LUT path).
  - Kept SQ_CMD policy framework and runtime trap/readback diagnostics.
- `drivers/gpu/drm/amd/amdkfd/kfd_pc_sampling.c`
  - Hosttrap session tracks `owner_pasid`.
  - PASID->VMID lookup (with fallback scan + miss/hit logs).
  - Trigger thread resolves VMID dynamically when `target_vmid==0`.
  - Added hosttrap start hook to force `program_trap_handler_settings()` for resolved VMID (new, pending validation after reboot).
- `drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c`
  - MES queue add and VMID map diagnostics.
- `drivers/gpu/drm/amd/amdkfd/kfd_device.c`
  - KFD probe log prints selected function table and callback pointers.

### Userspace (`~/rocm-systems`, `projects/rocr-runtime`)
- `trap_handler.s`
  - Kept gfx11 TMA decode fix (`GET_TMA << 8` + sign-extend).
  - Removed state-mutating debug writes.
- `amd_gpu_agent.cpp`
  - gfx11.5 blob selection generalized (`major=11 && minor=5`).
  - Kept focused trap setup/flush logs.

## High-Value Experiment Log
| run_tag | policy | Key result | Decision impact |
|---|---:|---|---|
| `20260207_110823` | 1 | VM fault consistent with bad TMA decode | Fixed userspace trap decode path |
| `20260207_114819` | 14 | Trap programmed/read back but no samples (`post_status=0`) | Problem is beyond basic trap setup |
| `20260207_132559` | 14 | Trigger on `vmid=0`, zero trap regs | Identified stale target VMID bug |
| `20260207_141516` | 14 | `get_atc_vmid_pasid_mapping_info is NULL` | Explained why VMID stayed 0 under MES |
| `20260207_142059` | 14 | Same NULL callback after reinstall | Required binary/deploy verification logs |
| `20260207_142958` | 14 | Callback active; VMID resolved to 8; runtime trap regs still zero; `post_status=0`; no CSV; timeout/hang | New primary fault zone: trap-register programming/effectiveness for resolved VMID |

## Current Findings
1. CSV generation failure is still pre-userspace-copy (trap never latches).
2. VMID targeting is now correct (resolved nonzero VMID observed).
3. Trigger executes (`SQ_CMD` written), but hosttrap pending bit never sets (`post_status=0`).
4. Runtime logs indicate trap regs appear unprogrammed for the VMID used by trigger.
5. Policy sweeps are low value until this trap-register mismatch is resolved.

## Next Plan (minimum reboot cost)
1. Keep fixed knobs (`policy=14`) and short timeout for every test.
2. Validate the new start-time explicit trap programming after reboot.
3. In one short run, require this log sequence:
   - `kfd probe: ... get_atc=<non-null>`
   - `pcs hosttrap: start resolved target_vmid=...`
   - `pcs hosttrap: start program trap regs vmid=... tba=... tma=...`
   - `trigger_pc_sample_trap: runtime_regs ... expected_valid=1` (expected after fix)
   - then either:
     - `post_status != 0` and CSV produced, or
     - still `post_status=0` (then investigate whether registers are overwritten/reset between programming and trigger).
4. If still failing with nonzero programmed regs:
   - instrument immediate pre-SQ_CMD and post-SQ_CMD trap-reg snapshots for same VMID.
   - instrument queue remap/add path to detect trap-reg overwrite or wrong VMID context switch.

## Reboot/Run Checklist (commands)
```bash
echo 14 | sudo tee /sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_sqcmd_policy
echo 64 | sudo tee /sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_max_injected_traps
echo 0 | sudo tee /sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_post_status_delay_us
echo 1 | sudo tee /sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_wave_scan_on_reset
echo 32 | sudo tee /sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_wave_scan_max_waves
echo 1 | sudo tee /sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_wave_target_debug
echo 1 | sudo tee /sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_runtime_reg_readback_on_reset

PCS_TIMEOUT_SEC=15 ./test_pc_sampling.sh

dmesg | rg "kfd probe:|start resolved target_vmid|start program trap regs|runtime_regs vmid=|tma_check vmid=|post_status=|pc_sampling_host_trap"
```
