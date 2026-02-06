# PC Sampling GFX1151 (Strix Halo APU) — Status & Plan

## Platform
- RDNA 3.5 APU, shared memory, CWSR disabled (`cwsr_enable=0`)
- `system_allocator()` for `device_data` (finegrain returns NULL on APU)
- MES manages VMIDs; TBA/TMA passed via `add_queue_mes`; `trap_en=1`

## Root Cause Found: SQ_CMD MODE_SINGLE Misses Waves

SRBM readback confirmed TBA/TMA registers are correctly programmed. But `post_status=0x0` after every SQ_CMD trigger — no wave was ever trapped.

- `SQ_IND_CMD_MODE_SINGLE` targets a specific WAVE_ID slot (0, 1, 2...)
- Active waves don't occupy the sequentially-scanned slots → SQ_CMD is a no-op
- **Fix applied**: Changed to `SQ_IND_CMD_MODE_BROADCAST` in `amdgpu_amdkfd_gfx_v11.c`
- Kernel module rebuilt and installed, awaiting reboot + test

## Current Diagnostic Build (Trap Handler)

Debug instrumentation in `trap_handler.s` to verify handler entry once broadcast works:

1. **Entry point** (`.is_host_trap_detected` GFX11 block):
   - Writes `0xBEEF` to raw ttmp[14:15]+8 (should be wrong addr)
   - Shifts ttmp[14:15] left by 8, sign-extends, writes `0xDEAD` to real TMA+8
   - Host reads `tma[1]` — expect `0xDEAD` if handler is reached

2. **Handler body** (`.profile_trap_handlers_gfx11`):
   - Writes `0xDEAD` to `reserved0` (offset 0x0C)
   - Writes `1` to `buf_write_val` (offset 0x00) via `global_store_b32` (not atomic)
   - Phase 1 atomic replaced with simple store to isolate VGPR overlap issue

3. **Host side** (`amd_gpu_agent.cpp`):
   - Direct memory read of header (APU shared mem, no DmaCopy)
   - Prints `reserved0`, `buf_write_val`, `tma[0]`, `tma[1]` on first 5 flushes

## Expected Test Results After Broadcast Fix

| Field | Expected | Meaning |
|-------|----------|---------|
| `post_status` | Non-zero | Wave(s) trapped |
| `tma[1]` | `0xDEAD` | Handler entry reached, TMA shift correct |
| `reserved0` | `0xDEAD` | Handler body reached |
| `buf_write_val` | Non-zero | Store to host_trap_buffers landed |

## Known Issues to Fix After Broadcast Works

1. **VGPR overlap in `global_atomic_add_u64`**: `v1` serves as VADDR AND part of VDATA/VDST `v[0:1]`. Need separate voffset register. Currently bypassed with `global_store_b32`.
2. **Debug instrumentation to remove**: TMA magic writes at entry, diagnostic stores in Phase 1, debug fprintf in host code, kernel printk's.

## Next Steps

1. Reboot, run `./test_pc_sampling.sh`, check table above
2. If broadcast works (all fields show expected values):
   a. Fix VGPR overlap: use v0 as voffset, v[1:2] as vdata for `global_atomic_add_u64`
   b. Restore proper Phase 1 atomic increment
   c. Remove all debug instrumentation (handler + host + kernel)
   d. Test end-to-end: verify samples land with correct PC/timestamp/HW_ID
3. If `post_status` still 0: investigate whether broadcast mode needs different SQ_CMD fields
4. If handler reached but stores don't land: TMA indirection or address issue

## GFX11 ISA Constraints
- No SMEM stores/atomics (`s_store_*`, `s_atomic_*`, `s_dcache_wb`)
- Use `global_store_b32`, `global_atomic_add_u64/u32` (VMEM SADDR mode)
- `HW_REG_HW_ID` → `HW_REG_HW_ID1`
- Timestamp: `s_sendmsg_rtn_b64 MSG_RTN_GET_REALTIME`
- Doorbell: `s_sendmsg_rtn_b32 MSG_RTN_GET_DOORBELL`
- SQ_SHADER_TMA stores address >> 8

## Key Files
- `projects/rocr-runtime/.../trap_handler/trap_handler.s` — trap handler
- `projects/rocr-runtime/.../runtime/amd_gpu_agent.cpp` — ROCr host-side PC sampling
- `amdgpu/drivers/gpu/drm/amd/amdkfd/kfd_process.c` — kernel TBA/TMA setup
- `amdgpu/.../amdgpu_amdkfd_gfx_v11.c` — SRBM programming + SQ_CMD trigger

## Build & Test
```
./build_rocr.sh && ./test_pc_sampling.sh
```
