# PC Sampling GFX1151 (Strix Halo APU) — Status & Plan

## Platform
- RDNA 3.5 APU (GFX11.5.1), shared memory, CWSR disabled, `amd_iommu=off`
- 2 SEs, 2 SHs/SE, 40 CUs, max_waves_per_simd=16, wave_front_size=32
- Linux 6.19, DKMS amdgpu module (6.16.13)

## Working Pipeline

1. **SQ_CMD** BROADCAST+CHECK_VMID+VM_ID (e.g. `0x80000495` for VMID=8) traps all waves
2. **Trap handler** saves PC to TTMP8/9, sleeps 500x`s_sleep 127` (~2ms), restores, `s_rfe_b64`
3. **Kernel reads** wave state via SQ_IND while waves sleep (STATUS, HW_ID2, TTMP0/1, PC_LO/HI)
4. **Filter**: STATUS.VALID + STATUS.PRIV + HW_ID2.VMID match
5. **Skip re-trap**: if waves still trapped from last SQ_CMD, read them without issuing new SQ_CMD

## Proven Constraints

### VMEM hangs in PRIV=1 (data path only)
- All data ops (`s_load`, `flat_*`, `global_*`) hang; instruction fetch works fine
- RDNA 3.5 has no `s_store_*`/`s_atomic_*` (invalid instructions)
- Workaround: kernel-side readback via SQ_IND instead of trap handler writing to buffer
- Future: test with `amd_iommu=on` + `cwsr_enable=1` to see if data path works

### Re-trap causes permanently stuck waves (FIXED)
- BROADCAST SQ_CMD sets pending trap on waves already in PRIV=1
- On `s_rfe_b64`, pending trap fires immediately before user code runs = infinite loop
- Clearing STATUS.TRAP in ttmp12 does NOT work (pending trap is in hardware latch, not STATUS)
- **Fix**: kernel skips SQ_CMD when `amdkfd_gfx11_last_trapped_count > 0`

### TTMP reads intermittently zero (~80%)
- GFX11 SQ_IND has no FORCE_READ bit (GFX9 has it at bit 13)
- Control regs (STATUS, HW_ID, PC_LO/HI) read reliably; TTMP/SGPR do not
- AUTO_INCR pair read + 3 retries implemented but yield still low
- curpc = TBA+0x94 (sleep loop) consistently confirms waves are in handler

### STATUS corruption on SQ_IND reads
- Calls 2+: ~122/128 wave slots return STATUS with bits 24+ set (reserved, must be 0)
- Filtered with `if (status & 0xFF000000) { corrupt_count++; continue; }`
- Only ~6 valid waves found on calls 2+ vs ~57 on call 1

### MES timeout on teardown
- Waves in trap handler prevent MES SUSPEND = "MES failed to respond to msg=SUSPEND"
- GPU reset (MODE2) succeeds thanks to `amdgpu_in_reset()` guards
- Happens during process exit, not during sampling
- Sampling thread has `amdgpu_in_reset()` in while loop condition

## Key Observations from Latest Test (2026-02-10)
- 25-second run, ~30K+ samples, test exited cleanly via SIGTERM
- Pattern: SQ_CMD → read (57 waves) → skip (0 waves, already exited) → SQ_CMD → read (6 waves) → skip (0) → ...
- Waves consistently return to user code between cycles (no stuck waves)
- Every other trigger call is wasted (skip finds 0 because 2ms sleep < 5ms interval)
- MES failure at teardown (~65s), GPU reset succeeded

## Next Steps

1. **Fix skip timing** — read trapped waves first, only issue SQ_CMD if none found. Eliminates wasted alternating calls.
2. **TTMP zero reads** — try halting waves (`s_sethalt 1` via SQ_CMD) before reading SGPR, or store PC in VGPR lane from trap handler and read VGPR via SQ_IND
3. **STATUS corruption** — investigate why only ~6/128 slots are valid on calls 2+ (possibly waves exit trap before read completes)
4. **Clean teardown** — stop sampling thread before queue teardown to avoid MES timeout
5. **Buffer management** — deliver samples to userspace (kernel-mapped buffer or copy-on-flush)
6. **Future: IOMMU + CWSR** — test standard trap handler approach with data memory enabled

## Key Files
- `projects/rocr-runtime/.../trap_handler/trap_handler.s` — trap handler (GFX11 path ~line 388)
- `amdgpu/.../amdgpu_amdkfd_gfx_v11.c` — trigger, wave read, PC readback
- `amdgpu/.../kfd_pc_sampling.c` — session management, sampling thread
- `amdgpu/.../kfd_device_queue_manager.c` — remap_queue MES path

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
