# PC Sampling GFX1151 (Strix Halo APU) — Status & Plan

## Platform
- RDNA 3.5 APU (GFX11.5.1), shared memory, CWSR disabled, `amd_iommu=off`
- 2 SEs, 2 SHs/SE, 40 CUs (20 WGPs), 4 SIMDs/WGP, max_waves_per_simd=16, wave32
- Linux 6.19, DKMS amdgpu module (6.16.13)

## Current Approach: Direct PC Reading

Read PC_LO/PC_HI directly from running waves via SQ_IND control registers.
No trapping, no SQ_CMD, no trap handler interaction during sampling.

### Wave iteration (per umr pattern)
```
for se in 0..num_se:
  for sh in 0..num_sh:
    for wgp in 0..max_cu_per_sh/2:
      for simd in 0..3:
        select_se_sh(se, sh, (wgp << 2) | simd)   # MANY_TO_INSTANCE
        for wave in 0..max_waves_per_simd-1:       # 0..15
          STATUS  → filter VALID, skip PRIV
          HW_ID2  → filter by VMID
          PC_LO/HI → sample
```

### Why this works
- Control registers (STATUS, HW_ID, PC_LO/HI) read reliably via SQ_IND
- TTMP/SGPR reads unreliable (~80% zero) on GFX11 — no FORCE_READ bit
- No trapping → no stuck waves, no MES teardown hang

## Key Register Details

### SQ_WAVE_STATUS (ixSQ_WAVE_STATUS=0x0102)

| Bit | Field | Notes |
|-----|-------|-------|
| 5 | PRIV | In trap handler — skip these (PC = handler, not user code) |
| 13 | HALT | Ignored while PRIV=1 (ISA confirmed) |
| 14 | TRAP | Trap **pending**, NOT active |
| 16 | VALID | Wave slot occupied |
| 22 | OREO_CONFLICT | Valid field |
| 23 | FATAL_HALT | Valid field |
| 24 | NO_VGPRS | Valid field |
| 25 | LDS_PARAM_READY | Valid field |
| 26 | MUST_GS_ALLOC | Valid field |
| 27 | MUST_EXPORT | Valid field |
| 28 | IDLE | No outstanding instructions |
| 29 | SCRATCH_EN | Scratch enabled |
| 30-31 | Reserved | Corruption mask: `0xC0000000` |

### SQ_IND_INDEX (regSQ_IND_INDEX=0x1118)
- WAVE_ID [4:0]: wave slot within selected SIMD (0-15 used)
- WORKITEM_ID [10:5]: thread selector
- AUTO_INCR [11]: auto-increment INDEX on SQ_IND_DATA read
- INDEX [16:31]: register offset
- **No FORCE_READ** (was bit 13 on GFX9, removed on GFX10+)

### GRBM_GFX_INDEX — Wave Addressing on GFX10+
- INSTANCE_INDEX (7 bits) encodes WGP + SIMD: `(wgp << 2) | simd`
- INSTANCE_BROADCAST_WRITES is a **write-mode** flag — wrong for reads
- Must iterate per-instance to read all SIMDs (confirmed by umr source)

### SQ_CMD (regSQ_CMD=0x111b)
- CMD [3:0], MODE [6:4], CHECK_VMID [7], DATA [11:8]
- WAVE_ID [20:16] (5 bits), QUEUE_ID [26:24], VM_ID [31:28]
- MODE=1 = BROADCAST, CMD=1 = SETHALT
- No SIMD_ID field on GFX11 (unlike GFX9)

### TBA/TMA Registers
- regSQ_SHADER_TBA_LO/HI, regSQ_SHADER_TMA_LO/HI
- Kernel stores address >> 8 (256-byte aligned)
- TRAP_EN in TBA_HI bit 31
- Trap handler reads TMA via `s_sendmsg_rtn_b64 MSG_RTN_GET_TMA` (0x82), then shifts left by 8

## VMEM Hang in PRIV=1 — Root Cause Analysis

### Symptoms
- All data ops (`s_load_dword`, `flat_*`, `global_*`) hang in trap handler context
- Instruction fetch works fine (trap handler executes from TBA)
- Workaround: trap handler does sleep-only loop; kernel reads PC via SQ_IND

### Evidence That Data Ops SHOULD Work in PRIV Mode
1. **ISA says no restrictions** — only TTMP writes and STATUS are PRIV-restricted
2. **CWSR handler uses data ops in PRIV mode on GFX11** — `s_load_dword` (line 320)
   and `global_store_dword_addtid` (lines 429, 461) in `cwsr_trap_handler_gfx10.asm`
3. **Same CWSR binary used for all GFX11.x** including GFX11.5
   (`kfd_device.c:552-557`: `IP_VERSION >= 11,0,0 && < 12,0,0` → `cwsr_trap_gfx11_hex`)
4. **GFX11 SH_MEM_CONFIG has no RETRY_DISABLE field** — retry is not controlled
   per-VMID, so the missing retry config in `set_cache_memory_policy_v11` is by design
5. **SH_MEM_CONFIG IS configured** for the VMID via `program_sh_mem_settings` in
   `allocate_vmid`, which is always called regardless of cwsr_enable
6. **`update_qpd_v11` is intentionally empty** — GFX11 doesn't need per-QPD updates

### Most Likely Root Cause: TMA Memory Mapping

The CWSR handler and our handler allocate TMA differently:

| | CWSR (cwsr_enable=1) | Our handler (cwsr_enable=0) |
|---|---|---|
| TBA alloc | `vm_mmap(RESERVED_MEM)` — KFD manages GPU PTEs | `system_allocator()` — kernarg pool |
| TMA location | Same alloc as TBA (`tba + KFD_CWSR_TMA_OFFSET`) | Separate `coarsegrain_allocator()` alloc |
| TMA mapping | Within TBA's mapped region — guaranteed accessible | `MakeMemoryResident()` + `allow_access()` |
| s_load target | TMA+0x10 (within TBA allocation) | TMA+0 (separate allocation) |

The CWSR handler's `s_load_dword` accesses TMA within the same allocation as TBA.
Since TBA works for instruction fetch, TMA data access also works. Our handler's
TMA is a separate `coarsegrain_allocator()` allocation that may lack valid GPU page
table entries for the wave's VMID.

### How to Verify
1. **Test `s_load_dword` from TBA address** (not TMA) — TBA is definitely mapped.
   If this works, PRIV mode data ops work and the hang is a TMA mapping issue.
2. **Allocate TMA within TBA region** — put device_data pointer at a known offset
   within the trap handler code buffer (like CWSR does).
3. **Enable `cwsr_enable=1`** — if standard CWSR handler works, data ops in PRIV
   mode work on GFX11.5 and the issue is mapping-specific.

### KFD Trap Handler Setup Flow (cwsr_enable=0)
1. ROCr `SetTrapHandler` → thunk `hsaKmtSetTrapHandler` → ioctl `SET_TRAP_HANDLER`
2. KFD `kfd_process_set_trap_handler`: since `cwsr_kaddr==NULL`, stores as first-level
   TBA/TMA in `qpd->tba_addr` / `qpd->tma_addr`
3. `allocate_vmid` calls `program_sh_mem_settings` (always) but skips
   `program_trap_handler_settings` (only if `cwsr_enabled`)
4. Our code calls `program_trap_handler_settings_v11` directly to program SQ_SHADER_TBA/TMA

## Proven Constraints

### No FORCE_READ → SGPR/TTMP reads unreliable
- ISA: wave must be "valid, halted and idle" for reliable SGPR reads
- HALT ignored while PRIV=1 → can't halt trapped waves for reads
- Control registers (STATUS, HW_ID, PC) read reliably regardless of wave state

### GFX11 ISA constraints
- No SMEM stores (`s_store_*`, `s_atomic_*`, `s_dcache_wb`)
- `HW_REG_HW_ID` → `HW_REG_HW_ID1`
- Timestamp: `s_sendmsg_rtn_b64 MSG_RTN_GET_REALTIME` (0x83)
- `s_sleep` uses bits [6:0] only — max 127
- TTMP0:1 only registers initialized at trap entry; TTMP2-15 NOT auto-initialized
- TBA/TMA must be read via `s_sendmsg_rtn_b64` (0x85/0x82), not `s_getreg`

## Next Steps

1. **Build & test** direct PC reading with per-instance wave iteration
2. **Verify VMEM hang root cause** — test `s_load_dword` from TBA address in trap handler
3. **Buffer management** — deliver samples to userspace
4. **Clean teardown** — verify no MES timeout with direct-read approach
5. **Reduce logging** — pr_warn → pr_debug for production
6. **Future: IOMMU + CWSR** — test standard trap handler approach with `cwsr_enable=1`

## Reference Documents
- `rdna35_instruction_set_architecture.md` — ISA manual (31K lines)
- `amdgpu_isa_rdna3_5.xml` — instruction encodings/opcodes (large)
- `~/amdgpu/.../include/asic_reg/gc/gc_11_5_0_sh_mask.h` — register bit fields
- `~/amdgpu/.../include/asic_reg/gc/gc_11_5_0_offset.h` — register offsets
- `~/umr/` — umr debugger source; key files: `src/lib/scan_waves.c`,
  `src/lib/sq_cmd_halt_waves.c`, `src/lib/mmio.c`

All large reference files: search with Grep, never read directly (exhausts context).

## Key Files
- `~/amdgpu/.../amdgpu_amdkfd_gfx_v11.c` — trigger, wave read, PC readback
- `~/amdgpu/.../kfd_pc_sampling.c` — session management, sampling thread
- `~/amdgpu/.../kfd_device_queue_manager.c` — remap_queue MES path
- `~/amdgpu/.../kfd_device_queue_manager_v11.c` — SH_MEM_CONFIG setup, empty update_qpd
- `~/amdgpu/.../kfd_process.c` — CWSR init, SetTrapHandler ioctl handler
- `~/amdgpu/.../cwsr_trap_handler_gfx10.asm` — CWSR handler (uses s_load + global_store in PRIV)
- `projects/rocr-runtime/.../trap_handler/trap_handler.s` — our trap handler (sleep-only workaround)
- `projects/rocr-runtime/.../amd_gpu_agent.cpp` — TBA/TMA allocation, SetTrapHandler call

## Build & Test
```bash
# Kernel (requires sudo + reboot):
cd ~/amdgpu && sudo dkms build amdgpu/1.0 && sudo dkms install amdgpu/1.0 --force
# ROCr (no reboot):
cd ~/rocm-systems && bash build_rocr.sh
# Test:
PCS_TIMEOUT_SEC=25 PCS_KERNEL_LAUNCHES=200000 bash test_pc_sampling.sh
# Check:
dmesg | grep -E "pcs_sample|read_pcs|trigger_pc|post_status"
```
