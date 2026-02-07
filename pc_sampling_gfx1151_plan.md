# PC Sampling gfx1151 (Strix Halo) - Debug Plan

## Goal
- Host-trap PC sampling on `gfx1151` should generate non-empty `*pc_sampling_host_trap*.csv` without hangs/faults.

## Current Blocker
- `SQ_DEBUG_HOST_TRAP_STATUS.pending_count` latches to `1` right after first accepted SQ_CMD and never clears.
- Trigger loop then always returns early (`skip SQ_CMD because pending_count=1`), so hosttrap buffer is never written and no CSV is produced.

## What Is Already Proven
- VMID resolution is working:
  - `get_atc_vmid_pasid_mapping_info` is wired for gfx11.
  - runtime resolves `owner_pasid=32770 -> target_vmid=8`.
- Trap regs are programmed correctly for vmid 8:
  - TBA/TMA intended vs readback match.
  - `SPI_GDBG_PER_VMID_CNTL.TRAP_EN=1`.
- Userspace trap blob path is not the current blocker:
  - gfx11 TMA decode fix is applied.
  - current failure is before userspace sees trap records.

## Key Experiments (high signal only)
| run_tag | policy | Key observation | Conclusion |
|---|---:|---|---|
| `20260207_132559` | 14 | `vmid=0` trigger path, zero trap regs | VMID resolution bug existed |
| `20260207_141516` | 14 | `get_atc_vmid_pasid_mapping_info is NULL` | Missing callback wiring found |
| `20260207_142958` | 14 | VMID resolves, still no samples | Problem moved to trigger/status path |
| `20260207_174218` | 14 | `pending_count=1` sticky + repeated skip SQ_CMD | Primary blocker identified |
| `20260207_184929` | 14 | Live queue/wave retarget (`queue 2`, `wave 0`) but immediate latch | Queue retarget alone insufficient |
| `20260207_190106` | 14 | First accepted SQ_CMD then `post_status=0x1`; status seen on SH0+SH1 though target wave in SH1 | Strong hint SQ_CMD scope issue (`SE/SH=all`) |

## Code State (only deltas that matter now)
### Kernel (`~/amdgpu`)
- `amdgpu_amdkfd_gfx_v11.c`
  - VMID lookup callback and VMID-aware diagnostics added.
  - Live target wave/queue picker added.
  - **New fix candidate**: for `MODE_SINGLE`, SQ_CMD now targets selected `SE/SH` instead of broadcast-all.
  - Added scope logs: `scope=single_se_sh|broadcast_all`, `scope_se`, `scope_sh`.
  - Added first-latch context logs (`last_sqcmd`, status matrix, global wave summary).
- `kfd_pc_sampling.c`
  - hosttrap session now tracks `owner_pasid` and resolved `target_vmid`, with explicit start/thread logs.

### Userspace (`~/rocm-systems/projects/rocr-runtime`)
- `trap_handler.s`: gfx11 TMA decode fix kept; state-mutating trap debug writes removed.

## Current Hypothesis
- `pending_count` should clear only after host trap service.
- With old behavior, `MODE_SINGLE` SQ_CMD was issued with `SE/SH=all`, potentially creating pending on non-target SHs with no service path.
- Any non-clearing SH pending blocks all future SQ_CMD globally in current loop.

## Next Plan
1. Rebuild + reinstall DKMS with current kernel changes (includes `SE/SH`-scoped SQ_CMD for `MODE_SINGLE`).
2. Reboot once, keep policy `14`, run a short clean test (no stale `pc_sampling_test`/`rocprofv3`).
3. Validate acceptance criteria:
   - first accepted SQ_CMD log shows `scope=single_se_sh`;
   - `pending_count` does not latch permanently right after first accepted SQ_CMD;
   - hosttrap CSV appears.
4. If still failing, do no-reboot A/B:
   - `amdkfd_gfx11_pcs_sqcmd_cmd_override=-1` vs `4` (`TRAP_AFTER_INST`),
   - `amdkfd_gfx11_pcs_single_wave_use_status_slot=0` vs `1`.

## Reboot/Run Checklist
```bash
echo 14 | sudo tee /sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_sqcmd_policy
echo 4096 | sudo tee /sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_max_injected_traps
echo 0 | sudo tee /sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_post_status_delay_us
echo 1 | sudo tee /sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_wave_scan_on_reset
echo 32 | sudo tee /sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_wave_scan_max_waves
echo 1 | sudo tee /sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_wave_target_debug
echo 1 | sudo tee /sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_runtime_reg_readback_on_reset
echo 0 | sudo tee /sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_single_wave_use_status_slot
echo -1 | sudo tee /sys/module/amdgpu/parameters/amdkfd_gfx11_pcs_sqcmd_cmd_override

cd /home/wd/rocm-systems
PCS_TIMEOUT_SEC=5 ./test_pc_sampling.sh

dmesg | rg "trigger_pc_sample_trap: (SQ_CMD|post_status|skip SQ_CMD|pending latch|scope=|retarget)|pcs hosttrap: start resolved target_vmid|pc_sampling_host_trap"
```
