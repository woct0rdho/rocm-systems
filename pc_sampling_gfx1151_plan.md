# PC Sampling GFX1151 (Strix Halo APU) — Status & Plan

## Platform
- RDNA 3.5 APU, shared memory, CWSR disabled (`cwsr_enable=0`)
- MES manages VMIDs; TBA/TMA passed via `add_queue_mes`; `trap_en=1`
- GFX11 SQ_CMD: no SIMD_ID field; WAVE_ID[20:16] is 5 bits (0-31)

## Root Causes Found

### 1. MES had stale TBA/TMA=0 (FIXED)
First 3 compute queues are created by ROCr BEFORE `UpdateTrapHandlerWithPCS` calls `SetTrapHandler`. At queue creation, `qpd->tma_addr=0`. MES caches TMA=0 from `add_queue_mes` and programs it into per-VMID registers on every remap, overwriting SRBM programming.

**Fix**: In `remap_queue()`, added MES path: `remove_all_kfd_queues_mes()` + `add_all_kfd_queues_mes()` so MES re-reads updated `qpd->tba_addr`/`qpd->tma_addr`. Called from `kfd_pc_sample_start()` before trigger thread starts.

**Evidence**: After fix, `add_queue_mes` logs show correct TBA/TMA for all re-added queues. First-ever non-zero `post_status=0x3` observed at count=1966.

### 2. Trap handler doesn't write samples (CURRENT)
Despite traps being delivered (`post_status=0x3`), `write_val=0` — the trap handler's `flat_atomic_add_x2` on `buf_write_val` never fires.

**Hypothesis**: MES overwrites TMA register between SQ_CMD delivery and the trap handler's `MSG_RTN_GET_TMA` call. The trap handler gets TMA=0, hits the null guard (line 416-417 of `trap_handler.s`), and exits without writing.

**Key question**: Does MES program TMA into per-VMID registers on remap, or only TBA? If MES ignores TMA, the SRBM-programmed value gets cleared on every remap.

## Trap Handler Flow (GFX11 path, `trap_handler.s`)
```
trap_entry:
  check HT bit in ttmp1 → .is_host_trap_detected
  MSG_RTN_GET_TMA → ttmp[2:3]
  shift left 8 → ttmp[14:15] = TMA byte address
  sign-extend if bit 47 set

.profile_trap_handlers_gfx11:
  save exec, v0-v3; exec=1 (lane 0 only)
  NULL GUARD: if TMA == 0 → skip to restore          ← likely failing here
  flat_load_dwordx2 from TMA → host_trap_buffers
  NULL GUARD: if host_trap_buffers == 0 → skip
  flat_atomic_add_x2 on buf_write_val                 ← this is write_val
  load buf_size, compute sample_addr
  flat_store_dword sample.pc_lo, sample.pc_hi
  flat_atomic_add on buf_written_val
  restore exec, v0-v3
```

## Kernel Changes (vs dkms baseline)

### Needed for PC sampling
- `get_atc_vmid_pasid_mapping_info_v11()` — VMID-to-PASID resolution
- `program_trap_handler_settings_v11()` — program TBA/TMA/GDBG via SRBM per-VMID
- `kgd_gfx_v11_trigger_pc_sample_trap()` — SQ_CMD trigger with SRBM reprogramming
- `kgd_gfx_v11_get_hosttrap_status()` — pending_count check before SQ_CMD
- Per-VMID address arrays for TBA/TMA reprogramming in trigger
- kfd2kgd table wiring
- `remap_queue()` MES path: remove+add all queues (forces MES to re-read TBA/TMA)
- `kfd_pc_sample_start()`: call `remap_queue` before trigger thread starts

### Trigger function design (matches GFX9/GFX12)
- Default policy: `MODE_SINGLE`, wave_id sweep 0-15, no `CHECK_VMID`, broadcast SE/SH
- `CMD=0x5` (SQ_IND_CMD_CMD_TRAP), `DATA=0x4` (HOSTTRAP trap ID)
- Pending_count skip (same as GFX9/GFX12)
- SRBM reprogramming of TBA/TMA before every SQ_CMD (may be removable once MES TMA issue resolved)
- Module params: `sqcmd_policy` (0=default, 1=broadcast, 2=noop), `post_status_delay_us`

### Debug helpers
- `kgd_gfx_v11_dump_wave_trap_state()` — per-wave TRAPSTS/STATUS/HW_ID dump
- `kgd_gfx_v11_log_runtime_trap_regs()` — SRBM readback of TBA/TMA
- `kgd_gfx_v11_log_hosttrap_status_matrix()` — status across all SE/SH
- `kgd_gfx_v11_log_tma_check()` — quick TMA readback

## Key Experiments
| Date | Change | Key observation |
|---|---|---|
| 20260207 | Old 16-policy system | `post_status=0x1` after first SQ_CMD, then latches |
| 20260208 | Simplified to 3 policies | `programmed_tma_reg=0x0`, MES overwrites TMA |
| 20260208 | SRBM reprogram before SQ_CMD | Readback correct, but `post_status=0x0` still |
| 20260208 | **remap_queue MES path** | `FIRST post_status=0x3` at count=1966 — traps delivered! |
| 20260208 | But `write_val=0` | Trap handler executes but doesn't write samples |

## Next Steps
1. **Determine if MES programs TMA**: Read TMA register AFTER MES remap (without SRBM reprogramming) to see if MES preserves TMA or clears it
2. **If MES doesn't program TMA**: Need alternative approach:
   - Option A: Use `SET_SHADER_DEBUGGER` MES command (but it doesn't pass TMA)
   - Option B: Store TMA in a GPU-accessible location the trap handler can read without MSG_RTN_GET_TMA
   - Option C: Patch the trap handler to get TMA from a fixed/known address instead of the register
3. **If MES does program TMA**: The issue is elsewhere — check flat_load faulting in trap context, or timing
4. Once samples land: verify CSV output, clean up debug logging

## Key Files
- `projects/rocr-runtime/.../trap_handler/trap_handler.s` — trap handler (GFX11 path at line 364)
- `projects/rocr-runtime/.../runtime/amd_gpu_agent.cpp` — `UpdateTrapHandlerWithPCS`, `BindTrapHandler`
- `amdgpu/drivers/gpu/drm/amd/amdkfd/kfd_pc_sampling.c` — kernel session/thread/VMID, remap call
- `amdgpu/.../amdgpu_amdkfd_gfx_v11.c` — SRBM programming + SQ_CMD trigger
- `amdgpu/.../kfd_device_queue_manager.c` — `remap_queue` MES path, `add_queue_mes`

## Build & Test
```
# Kernel module (requires sudo):
cd ~/amdgpu && sudo dkms build amdgpu/6.16.13 && sudo dkms install amdgpu/6.16.13 --force
# Reboot, then:
./test_pc_sampling.sh
```
