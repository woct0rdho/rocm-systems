# PC Sampling GFX1151 (Strix Halo APU) — Status & Plan

## Platform
- RDNA 3.5 APU (GFX11.5.1), shared memory, CWSR disabled (`cwsr_enable=0`)
- 2 SEs, 2 SHs/SE, 80 SIMDs, 40 CUs, max_waves_per_simd=16, wave_front_size=32
- MES manages VMIDs; TBA/TMA passed via `add_queue_mes`; `trap_en=1`
- GFX11 SQ_CMD: no SIMD_ID field; broadcast SE/SH targets all SIMDs

## Confirmed Working

### 1. MES TBA/TMA remap (FIXED)
Early queues created before `SetTrapHandler` had TMA=0. Fixed by calling `remap_queue()` (remove+add all MES queues) in `kfd_pc_sample_start()` so MES re-reads updated `qpd->tba_addr`/`qpd->tma_addr`.

### 2. SET_SHADER_DEBUGGER for TRAP_EN (FIXED)
PC sampling path never called `kfd_dbg_set_mes_debug_mode()`, so MES didn't persistently set `SPI_GDBG_PER_VMID_CNTL.TRAP_EN=1`. Added call before remap in `kfd_pc_sampling.c`. Without this, SQ_CMD TRAP was ignored for ~9s until a race condition briefly enabled it.

### 3. SRBM reprogramming removed (FIXED)
Removed ~40 lines of SRBM TBA/TMA/GDBG reprogramming from trigger path. No longer needed since MES has correct values via remap. Trigger now matches GFX12: lock mutex → broadcast SE/SH → write SQ_CMD → unlock.

### 4. Traps ARE being delivered (CONFIRMED)
`post_status` shows 5-16 waves entering host trap handler. `SET_SHADER_DEBUGGER` was the key fix. Traps consistently appear ~10s into test (when GPU workload is running).

## Current Blocker: VMEM hangs in trap context on GFX11.5

### Evidence
- Waves enter trap handler (post_status=0x5/0xd/0x10) but stay stuck for ~2 seconds
- Waves only exit when workload completes (queue cleanup kills them)
- `buf_write_val` is never incremented — trap handler never reaches the atomic add
- Tested both `flat_*` and `global_*` instructions — both hang identically
- Known issue: `s_load` also faults in trap context on gfx1151 (comment in trap_handler.s)
- **All three memory access methods (scalar, flat, global) fail in trap context on GFX11.5**

### Trap handler flow (trap_handler.s, GFX11 path)
```
trap_entry → check HT bit → .is_host_trap_detected
  MSG_RTN_GET_TMA → shift left 8 → TMA VA in ttmp[14:15]
.profile_trap_handlers_gfx11:
  save state; exec=1 (lane 0)
  NULL guard TMA → bail if 0
  global_load_dwordx2 from TMA → host_trap_buffers    ← HANGS HERE
  NULL guard host_trap_buffers → bail if 0
  global_atomic_add_x2 on buf_write_val
  load buf_size, compute sample_addr
  global_store_dword sample.pc_lo/hi
  global_atomic_add on buf_written_val
  restore state
```

### Root cause hypothesis
GPU page table walks do not complete in trap context on GFX11.5. The TMA/buffer memory is mapped and accessible from normal shaders, but TLB misses in trap context cause the wave to hang indefinitely. This is a hardware limitation.

## Next Plan: Kernel-side PC readback

Since the trap handler cannot access memory on GFX11.5, bypass it entirely. Read the trapped wave's PC directly from hardware registers in the kernel trigger function.

### Approach
When `post_status != 0` (waves are in trap handler), use `SQ_IND_INDEX`/`SQ_IND_DATA` to read wave state:

1. After SQ_CMD, poll `SQ_DEBUG_HOST_TRAP_STATUS` for `PENDING_COUNT > 0`
2. For each SE/SH with pending waves, iterate wave slots and read:
   - `ixSQ_WAVE_STATUS` (0x0102) — check if wave is valid and trapped
   - `ixSQ_WAVE_HW_ID2` (0x0118) — check VM_ID matches target
   - `ixSQ_WAVE_PC_LO` (0x0108) / `ixSQ_WAVE_PC_HI` (0x0109) — current PC (trap handler PC)
   - `ixSQ_WAVE_TTMP0` (0x026c) / `ixSQ_WAVE_TTMP1` (0x026d) — original PC saved at trap entry
3. Write PC samples to a kernel-managed buffer (or directly to the userspace buffer via kernel mapping)
4. The wave still needs to exit the trap handler — may need to modify trap handler to skip VMEM and just return

### Key functions
- `kgd_gfx_v11_wave_read_ind()` — reads wave state via SQ_IND_INDEX/SQ_IND_DATA
- `kgd_gfx_v11_dump_wave_trap_state()` — existing wave dump (reads TRAPSTS, STATUS, HW_ID, PC, TTMPs)

### Open questions
1. Can we read TTMP0/TTMP1 (original PC) from waves stuck in the trap handler? The waves are hung on a VMEM instruction — are the TTMPs readable?
2. How to make the trap handler exit without VMEM? Options:
   - Modify trap handler to skip all VMEM in GFX11.5 path (just restore and exit)
   - Use SQ_CMD to halt/kill the wave after reading PC (but this kills the user wave)
   - Accept the 2s hang and let queue cleanup kill the waves (wasteful but functional)
3. Buffer management: kernel writes PC samples — how to get them to userspace? Options:
   - Write to a kernel-allocated buffer, copy to userspace on flush
   - Map the userspace buffer into kernel space
   - Use the existing hosttrap buffer format but write from kernel

### Alternative: Fix trap handler with non-VMEM writes
If there's a way to write to memory from trap context without VMEM (e.g., via `s_sendmsg` to write to a hardware queue, or via MMIO-mapped memory), the trap handler could still work. Research needed.

## Kernel Changes (current state)

### Core PC sampling support
- `get_atc_vmid_pasid_mapping_info_v11()` — VMID-to-PASID resolution
- `program_trap_handler_settings_v11()` — program TBA/TMA/GDBG via SRBM
- `kgd_gfx_v11_trigger_pc_sample_trap()` — SQ_CMD trigger (simplified, no SRBM)
- `kgd_gfx_v11_get_hosttrap_status()` — pending_count check
- `remap_queue()` MES path in `kfd_device_queue_manager.c`
- `kfd_dbg_set_mes_debug_mode(pdd, true)` call in `kfd_pc_sampling.c`
- kfd2kgd table wiring

### Diagnostics (in current module)
- `kgd_gfx_v11_dump_wave_trap_state()` — per-wave register dump on first trap
- `kgd_gfx_v11_log_runtime_trap_regs()` — SRBM readback of TBA/TMA/GDBG
- FIRST post_status diagnostic with register readback
- Status transition logging

### Known bugs
- Trigger thread doesn't stop when process exits (caused GPU reset in test 1)
- `ever_trapped` static bool doesn't reset between sessions (FIRST diagnostic only fires once per module load)

## Trap handler change (rocr-runtime)
Changed `flat_*` → `global_*` in GFX11 path of `trap_handler.s` (lines 419-489). Did NOT fix the hang — both instruction types fail in trap context on GFX11.5. Change is still in place but irrelevant.

## Key Files
- `projects/rocr-runtime/.../trap_handler/trap_handler.s` — GPU trap handler (GFX11 path line 388)
- `amdgpu/.../amdgpu_amdkfd_gfx_v11.c` — trigger, wave dump, SRBM programming
- `amdgpu/.../kfd_pc_sampling.c` — session management, set_mes_debug_mode call, remap
- `amdgpu/.../kfd_device_queue_manager.c` — remap_queue MES path

## Build & Test
```bash
# Kernel module:
cd ~/amdgpu && sudo dkms build amdgpu/6.16.13 && sudo dkms install amdgpu/6.16.13 --force
# Reboot required (iGPU)

# ROCr runtime (no reboot needed):
bash build_rocr.sh

# Test:
PCS_TIMEOUT_SEC=15 bash test_pc_sampling.sh
# Check: dmesg | grep -E "post_status|status transition|set_mes_debug"
```
