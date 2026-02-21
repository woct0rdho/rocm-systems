# PC Sampling on gfx1151 (Strix Halo)

## Platform

- RDNA 3.5 APU (GFX 11.5.1), shared memory, `amd_iommu=off`, `amdgpu.cwsr_enable=0`
- 2 SEs, 2 SHs/SE, 40 CUs (20 WGPs), 4 SIMDs/WGP, max_waves_per_simd=16, wave32
- Linux 6.19, DKMS amdgpu module (6.16.13)

## Architecture: Direct PC Reading

Read PC_LO/PC_HI from running waves via SQ_IND. No trapping, no SQ_CMD.

### How It Differs from Normal PC Sampling (GFX9/12)

On GFX9/12, the **GPU hardware** traps one wave every N instructions/cycles. The trap
handler assembly writes one 64-byte sample into the device buffer and increments
`buf_written_val`. At the 80% watermark, GPU decrements `done_sig` to wake the ROCr
`PcSamplingThread`. Samples trickle in at hardware-controlled rate.

On GFX11.5, the **kernel thread** scans all running waves every 5ms via SQ_IND,
producing ~900 samples per scan. The kernel writes directly to the device buffer
from CPU via `kthread_use_mm`. There is no GPU signaling - `buf_written_val` is
never set by the GPU (ROCr pre-sets it to satisfy PM4 WAIT_REG_MEM). The ROCr
PcSamplingThread eager-drains any available data instead of waiting for a 4 MB
threshold.

### Wave Iteration (kernel: `read_wave_pcs`)

```
for se in 0..num_se:
  for sh in 0..num_sh:
    for wgp in 0..max_cu_per_sh/2:
      for simd in 0..3:
        GRBM_GFX_INDEX: INSTANCE = (wgp << 2) | simd
        for wave in 0..15:
          STATUS -> filter VALID, skip PRIV, skip corruption (bits 30-31)
          HW_ID2 -> filter by VMID, skip 0xbebebeef
          PC_LO/HI -> skip if either == 0xbebebeef
          -> fill kfd_pcs_sample: pc, hw_id=HW_ID2, timestamp=ktime_get_raw_ns()
```

### Kernel Thread Lifecycle (critical for correctness)

```
Init:
  kfd_lookup_process_by_pasid -> get_task_struct(lead_thread) -> kfd_unref_process
  get_task_mm -> kthread_use_mm -> read TMA[0], buf_size -> kthread_unuse_mm -> mmput
  (NO long-lived mm or process ref - prevents circular dependency)

Loop:
  read_wave_pcs(sample_buf) -> if samples > 0:
    get_task_mm(lead_thread) -> if NULL: process died, break
    kthread_use_mm -> pcs_write_to_device_data -> kthread_unuse_mm -> mmput

Exit triggers:
  - kthread_should_stop() - normal stop via kfd_pc_sample_stop ioctl
  - get_task_mm returns NULL - process exited
  - amdgpu_in_reset - GPU reset
```

### Why Per-Iteration MM Acquire/Release is Required

Holding `mm_users` across iterations prevents the ONLY KFD cleanup path:
```
Thread holds mm_users -> exit_mmap blocked -> mmu_notifier_release blocked ->
kfd_process_notifier_release never fires -> kfd_process_wq_release never runs ->
kfd_process_destroy_pdds never runs -> kfd_pc_sample_release never called ->
kthread_stop never called -> DEADLOCK
```

### ROCr Buffer Pipeline

```
Stage                          Size         Record    Capacity
─────────────────────────────────────────────────────────────
Device buffer (trap_buffer)    2 MB         64 B      32,768 samples
Host buffer (ROCr)             8 MB         64 B      131,072 samples
HandleSampleData threshold     4 MB / any   64 B      65K / 1 sample (stochastic / host_trap)
SDK buffer (tool.cpp)          64 KB        88 B      744 records
```

For host_trap, PcSamplingThread uses eager drain (`while (bytes > 0)` with
`min(bytes, session.buffer_size())` chunks). Buffer policy is LOSSLESS.

### Device Data Buffer Layout (`pcs_sampling_data_t`)

```
offset  field              size    notes
0x00    buf_write_val      8       (buf_idx<<63) | sample_count
0x08    buf_size           4       samples per buffer (observed: 32768)
0x0C    reserved0          4
0x10    buf_written_val0   4       Must be pre-set for host_trap method
0x14    buf_watermark0     4
0x18    done_sig0          8
0x20    buf_written_val1   4       Must be pre-set for host_trap method
0x24    buf_watermark1     4
0x28    done_sig1          8
0x30    reserved1          16
0x40    buffer0[]          buf_size * 64 bytes
        buffer1[]          buf_size * 64 bytes
```

### Sample Format: `kfd_pcs_sample` / `perf_sample_hosttrap_v1_t` (64 bytes)

```
offset  field              source
0x00    pc (u64)           SQ_IND PC_HI:PC_LO
0x08    exec_mask (u64)    0 (can't read reliably)
0x10    workgroup_id[3]    0 (TTMP unreliable)
0x1C    chiplet_info       0
0x20    hw_id (u32)        SQ_IND HW_ID2
0x24    reserved0          0
0x28    reserved1 (u64)    0
0x30    timestamp (u64)    ktime_get_raw_ns()
0x38    correlation_id     0
```

### ROCr Shutdown Sequence

```
registration::finalize:
  ...
  pc_sampling::stop_sampling_threads()   <- stops PcSamplingThread
    -> PcSamplingStop -> session.stop(), hsaKmtPcSamplingStop, signal, WaitForThread
    -> flush_internal_agent_buffers      <- flushes remaining samples
  pc_sampling::code_object::finalize()   <- no-op (thread stopped)
  pc_sampling::service_fini()            <- no-op
  code_object::finalize()
  invoke_client_finalizers()             <- tool_fini -> stop_context -> CSV output
  internal_threading::finalize()
```

## Bugs Fixed

### 1. PM4 WAIT_REG_MEM Hang (ROCr)

PM4 `WAIT_REG_MEM` polls `buf_written_val` to equal `old_val`. GPU trap handler
normally increments it, but kernel direct-read never does. Fix: pre-set
`buf_written_val[which_buffer] = old_val` for host_trap before PM4 submission.

### 2. Finalization Order Deadlock (ROCProfiler)

`code_object::finalize()` needs `host_buffer_mutex`, but PcSamplingThread holds it.
Original order called `code_object::finalize()` before stopping the thread. Fix: added
`pc_sampling::stop_sampling_threads()` in registration.cpp, called before
`code_object::finalize()`.

### 3. LOSSLESS Buffer Deadlock / Low Retention (ROCProfiler + ROCr)

PcSamplingThread only called `HandleSampleData` when host buffer reached 4 MB. Our
~20K samples (1.25 MB) never hit that threshold. All data sat until shutdown flush,
then burst-delivered to the 744-record SDK buffer. With LOSSLESS, emplace blocked and
deadlocked. With DISCARD, 96.5% of samples were dropped.

Fix: For host_trap, PcSamplingThread now eager-drains (`while (bytes > 0)`) instead
of batching to 4 MB. With incremental flow, LOSSLESS works without deadlock.

## Known Limitations

- **Race with ROCr flush** - kernel CPU writes vs ROCr PM4 GPU swaps on `buf_write_val`.
  Benign for statistical sampling (may lose a few samples at swap boundaries).
- **Single-XCC only** - `for_each_inst` loop overwrites `n_samples`. Fine for Strix Halo.
- **Dispatch_Id / Correlation_Id / Exec_Mask = 0** - expected for direct-read approach.
- **~20% sentinel corruption** - `0xbebebeef` in STATUS bits 30-31, HW_ID2, or PC.
  Filtered in `read_wave_pcs`.

## Hardware Constraints

### GFX11 SQ_IND - No FORCE_READ

- Control registers (STATUS, HW_ID, PC) read reliably
- TTMP/SGPR reads return zero ~80% of time - unusable
- GRBM_GFX_INDEX INSTANCE = `(wgp << 2) | simd`

### GFX11 ISA

- No SMEM stores (`s_store_*`, `s_atomic_*`, `s_dcache_wb`)
- `s_sleep` max 127; `HW_REG_HW_ID` -> `HW_REG_HW_ID1`

### VMEM Hang in PRIV=1 (for future reference)

Most likely a TMA memory mapping issue. CWSR handler's `s_load_dword` +
`global_store_dword_addtid` work in PRIV mode on GFX11, but our TMA likely lacks
valid GPU PTEs for data access (instruction fetch works at same VA range).

## Key Files

### Kernel (`~/amdgpu/`)

- `.../amdgpu_amdkfd_gfx_v11.c` - `read_wave_pcs`, `program_trap_handler_settings_v11`, `get_atc_vmid_pasid_mapping_info_v11`
- `.../kfd_pc_sampling.c` - sampling thread, `pcs_write_to_device_data`, VMID lookup, session lifecycle
- `.../kfd_priv.h` - `kfd_dev_pcs_hosttrap` fields
- `.../include/kgd_kfd_interface.h` - `kfd_pcs_sample` struct, `read_wave_pcs` callback
- `.../kfd_device_queue_manager.c` - `allocate_vmid` target_vmid refresh, `remap_queue` MES path

### ROCr (`projects/rocr-runtime/`)

- `.../amd_gpu_agent.cpp` - device_data alloc (system_allocator + MakeMemoryResident), TMA setup (coarsegrain + GPU VA), PcSamplingThread (5ms timeout for host_trap, eager drain), flush (buf_written_val pre-set), defensive checks
- `.../core/inc/amd_gpu_agent.h` - `pcs_sampling_data_t` struct, `device_data_size`, `device_data_gpu_va`
- `.../trap_handler/trap_handler.s` - GFX11 host trap entry, TMA decode, sleep loop for kernel PC read
- `.../pcs/pcs_runtime.h` - `PcSamplingSession`, `buffer_size()`, `sample_size()`

### ROCProfiler (`projects/rocprofiler-sdk/`)

- `.../tool.cpp` - SDK buffer size, buffer policy (LOSSLESS), null payload checks
- `.../pc_sampling/service.cpp` - `stop_sampling_threads()`
- `.../registration.cpp` - finalization order (stop_sampling_threads before code_object::finalize)
- `.../pc_sampling/hsa_adapter.cpp` - data_size validation
- `.../pc_sampling/ioctl/ioctl_adapter.cpp` - gfx11 enablement
- `.../pc_sampling/parser/translation.hpp` - GFX11 HW_ID2 parsing, copySample
- `.../pc_sampling/utils.hpp` - HSA buffer size (4 MB)

## Build & Test

```bash
# Kernel (requires sudo + reboot):
cd ~/amdgpu && sudo dkms build amdgpu/1.0 && sudo dkms install amdgpu/1.0 --force
# ROCr (no reboot):
cd ~/rocm-systems && bash build_rocr.sh
# ROCProfiler (no reboot):
cd ~/rocm-systems && bash build_rocprofiler.sh
# Test:
PCS_TIMEOUT_SEC=30 PCS_KERNEL_LAUNCHES=2000 bash test_pc_sampling.sh
# Check kernel logs:
dmesg | grep -E "pcs:"
```

## Next Steps

### Verify with Larger Workloads

- Test with PCS_KERNEL_LAUNCHES=200000
- Verify sample distribution across instructions
- Test with multiple concurrent workloads

### Condition Changes on gfx1151 (if other-GPU testing shows regressions)

The following changes currently apply to all GPUs that use PC sampling.
If dGPU (GFX9/GFX12) testing shows regressions, condition on ISA version.

#### MEDIUM RISK - Kernel

- **remap_queue MES path** - For MES GPUs (GFX11+, GFX12), remap now does
  `remove_all + add_all` instead of no-op. Only triggered during PC sampling
  start. Could cause brief queue interruption on GFX12 if it starts PC sampling
  via hosttrap.
- **kfd_dbg_set_mes_debug_mode** - Called for ALL MES GPUs during hosttrap PC
  sampling start. Enables `SPI_GDBG_PER_VMID_CNTL.TRAP_EN` persistently. Could
  affect GFX12 hosttrap behavior.
- **target_vmid instead of last_vmid_kfd** - The fallback `trigger_pc_sample_trap`
  path now passes the resolved VMID instead of `last_vmid_kfd`. More correct
  targeting, but behavior change for GFX9/GFX12. If VMID resolution fails
  (returns 0), the trap is skipped entirely - original code would have used
  `last_vmid_kfd` and triggered anyway.
- **program_trap_handler_settings in thread loop** - Called when
  `trap_regs_programmed_vmid != target_vmid`. For GFX9/GFX12 this is a redundant
  but harmless re-programming of TBA/TMA. Acquires SRBM lock briefly, once per
  VMID change.

#### MEDIUM RISK - ROCr

- **TMA allocation** - `finegrain_allocator` -> `coarsegrain_allocator` +
  `MakeMemoryResident` + GPU VA resolution in `UpdateTrapHandlerWithPCS`.
  `allow_access` now includes GPU agent (was CPU-only). Deallocation adds
  `MakeMemoryUnresident`. On dGPU, `MakeMemoryResident` may be a no-op and
  `agentBaseAddress` likely matches host VA, but untested.
- **device_data allocation** - `finegrain_allocator` -> `system_allocator` with
  0x1000 alignment + `MakeMemoryResident` in `PcSamplingCreateFromId`. Same
  deallocation change in `PcSamplingDestroy`.
