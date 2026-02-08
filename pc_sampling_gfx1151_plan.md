# PC Sampling GFX1151 (Strix Halo APU) — Status & Plan

## Platform
- RDNA 3.5 APU, shared memory, CWSR disabled (`cwsr_enable=0`)
- `system_allocator()` for `device_data` (finegrain returns NULL on APU)
- MES manages VMIDs; TBA/TMA passed via `add_queue_mes`; `trap_en=1`

## Root Cause Found: MES Overwrites TBA/TMA Registers

SRBM programming of SQ_SHADER_TBA/TMA succeeds (readback matches immediately after write), but MES overwrites them within milliseconds. By the time SQ_CMD fires, the trap handler reads TMA=0 via `s_sendmsg_rtn_b64 MSG_RTN_GET_TMA`, hits the null guard, and exits without writing samples.

### Evidence
- `program_trap_handler_settings_v11`: readback matches intended values immediately after SRBM write
- `tma_check` in trigger function (4ms later): `programmed_tma_reg=0x0`
- `post_status=0x0` always — no trap fires because TMA is zero

### Fix Applied
In `kgd_gfx_v11_trigger_pc_sample_trap()`, reprogram TBA/TMA/GDBG via SRBM right before every SQ_CMD write. This counteracts MES overwriting the registers.

## Kernel Changes (vs dkms baseline)

### Needed for PC sampling
- `get_atc_vmid_pasid_mapping_info_v11()` — VMID-to-PASID resolution (was NULL for GFX11)
- `program_trap_handler_settings_v11()` — program TBA/TMA/GDBG via SRBM per-VMID
- `kgd_gfx_v11_trigger_pc_sample_trap()` — SQ_CMD trigger with TBA/TMA reprogramming
- `kgd_gfx_v11_get_hosttrap_status()` — pending_count check before SQ_CMD
- Per-VMID address arrays for TBA/TMA reprogramming in trigger
- kfd2kgd table wiring

### Trigger function design (matches GFX9/GFX12)
- Default policy: `MODE_SINGLE`, wave_id sweep 0-15, no `CHECK_VMID`, broadcast SE/SH
- Pending_count skip (same as GFX9/GFX12)
- TBA/TMA SRBM reprogramming before every SQ_CMD (GFX11-specific, counteracts MES)
- `DATA=0x4` (HOSTTRAP trap ID, same as GFX9/GFX12)
- Module params: `sqcmd_policy` (0=default, 1=broadcast, 2=noop), `post_status_delay_us`

### Debug helpers (kept for future use)
- `kgd_gfx_v11_dump_wave_trap_state()` — per-wave TRAPSTS/STATUS/HW_ID dump
- `kgd_gfx_v11_log_runtime_trap_regs()` — SRBM readback of TBA/TMA (called on reset)
- `kgd_gfx_v11_log_hosttrap_status_matrix()` — status across all SE/SH
- `kgd_gfx_v11_log_tma_check()` — quick TMA readback

## Key Experiments
| Date | Policy | Key observation | Conclusion |
|---|---:|---|---|
| 20260207 | 14 | `post_status=0x1` after first SQ_CMD, then latches | SQ_CMD scope issue (old policy) |
| 20260208 | 14 | `programmed_tma_reg=0x0`, 1/4096 triggers found wave | MES overwrites TMA |
| 20260208 | 2 | 4096 broadcast SQ_CMDs, all `post_status=0x0` | Confirms TMA=0 is the blocker |

## Trap Handler State
- Uses `flat_*` instructions (no SMEM stores on GFX11)
- Saves/restores v0-v3, exec, ttmp registers
- Null guards on TMA and host_trap_buffers
- `flat_atomic_add_x2` for buf_write_val, `flat_atomic_add` for buf_written_val
- `flat_store_dword` for sample.pc
- TMA decode: `s_sendmsg_rtn_b64 MSG_RTN_GET_TMA` → shift left 8 → sign-extend if needed

## Next Steps
1. Rebuild + reinstall DKMS module with TBA/TMA reprogramming fix
2. Reboot, run test (default policy 0 = GFX12-style)
3. Expected: `tma_check` shows correct TMA, `post_status` non-zero, samples land
4. If samples land: verify CSV output with correct PC/timestamp data
5. Clean up debug logging (kernel + userspace)

## GFX11 ISA Constraints
- No SMEM stores/atomics (`s_store_*`, `s_atomic_*`, `s_dcache_wb`)
- Use `flat_*` or `global_*` (VMEM) for all stores/atomics
- `HW_REG_HW_ID` → `HW_REG_HW_ID1`
- Timestamp: `s_sendmsg_rtn_b64 MSG_RTN_GET_REALTIME`
- Doorbell: `s_sendmsg_rtn_b32 MSG_RTN_GET_DOORBELL`
- SQ_SHADER_TMA stores address >> 8
- SQ_CMD is write-only (readback returns garbage)

## Key Files
- `projects/rocr-runtime/.../trap_handler/trap_handler.s` — trap handler
- `projects/rocr-runtime/.../runtime/amd_gpu_agent.cpp` — ROCr host-side PC sampling
- `amdgpu/drivers/gpu/drm/amd/amdkfd/kfd_pc_sampling.c` — kernel session/thread/VMID
- `amdgpu/.../amdgpu_amdkfd_gfx_v11.c` — SRBM programming + SQ_CMD trigger

## Build & Test
```
# Kernel module (requires sudo):
cd ~/amdgpu && sudo dkms build amdgpu/6.16.13 && sudo dkms install amdgpu/6.16.13 --force
# Reboot, then:
./test_pc_sampling.sh
```
