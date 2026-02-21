# PC Sampling GFX1151 (Strix Halo APU) — Status & Plan

## Platform
- RDNA 3.5 APU (GFX11.5.1), shared memory, CWSR disabled, `amd_iommu=off`
- 2 SEs, 2 SHs/SE, 40 CUs (20 WGPs), 4 SIMDs/WGP, max_waves_per_simd=16, wave32
- Linux 6.19, DKMS amdgpu module (6.16.13)

## Current Status: 100% Sample Retention, Full Pipeline Working

### What Works (verified 2026-02-21)
- **Direct PC reading** via SQ_IND — ~900 VMID-matched samples/scan
- **Kernel→userspace delivery** — kernel writes to `device_data` via `kthread_use_mm`
- **ROCr picks up samples** — `PcSamplingFlushDeviceBuffers` reads samples
- **Incremental delivery** — PcSamplingThread eager-drains host buffer for host_trap method
- **Thread cleanup on process exit** — per-iteration mm acquire/release, no circular ref
- **~20% sentinel corruption** filtered (pc_lo/pc_hi/hw_id2 == `0xbebebeef`)
- **CSV output** — rocprofv3 writes decoded PC sampling CSV with instruction disassembly
- **Clean shutdown** — no SIGKILL, finalization completes within timeout
- **LOSSLESS buffer policy** — no dropped samples, no deadlocks

### Test Results (2026-02-21, after incremental delivery fix)
```
Kernel: total_delivered=20526 loops=36
PcSamplingThread incremental delivery:
  loop 5: 1,555 samples
  loop 6: 4,129 samples
  loop 7: 7,465 samples
  shutdown flush: 7,377 samples
CSV: 20,527 lines (20,526 samples + header)
Instructions decoded: s_delay_alu, v_add_co_u32, s_cbranch_scc0, s_cmp_eq_u32, etc.
Finalization: completes in <1s after user main() returns
Sample retention: 100% (20,526 of 20,526) — 0 "buffer too small" errors
```

### Previous Test Results (before incremental delivery fix)
```
Kernel: total_delivered=19955 loops=28
CSV: 699 lines (698 samples + header)
Sample retention: ~3.5% (699 of ~20K) — all delivered at shutdown, SDK buffer overflow
```

### Bugs Fixed (2026-02-21)

#### 1. LOSSLESS Buffer Deadlock
**Symptom**: PcSamplingThread hangs in `buffer::emplace` when SDK buffer is full.
**Root cause**: `ROCPROFILER_BUFFER_POLICY_LOSSLESS` blocks in emplace waiting for
`buffer::flush` (via thread pool → `join()`), while PcSamplingThread holds
`host_buffer_mutex`. Main thread's finalization path tries same mutex → deadlock.
**Fix**: Changed to `ROCPROFILER_BUFFER_POLICY_DISCARD` in tool.cpp:1777.
**Status**: Temporary workaround. Should revert to LOSSLESS once incremental delivery
is fixed (see next steps).

#### 2. Finalization Order Deadlock
**Symptom**: `code_object::finalize()` → `PcSamplingFlush()` hangs acquiring
`host_buffer_mutex` because PcSamplingThread is still running.
**Root cause**: Finalization order called `code_object::finalize()` (needs mutex) before
`invoke_client_finalizers()` (stops thread via `PcSamplingStop()`).
**Fix**: Added `pc_sampling::stop_sampling_threads()` in registration.cpp, called before
`code_object::finalize()`. Stops PcSamplingThread so flush can acquire mutex.

#### 3. PM4 WAIT_REG_MEM Hang
**Symptom**: `PcSamplingFlushDeviceBuffers` hangs on PM4 submission after finding samples.
**Root cause**: PM4 `WAIT_REG_MEM` polls `buf_written_val` to equal `old_val`. The GPU trap
handler normally increments `buf_written_val` per sample. But with kernel direct-read,
the kernel writes samples directly from CPU — `buf_written_val` is never set.
**Fix**: Pre-set `buf_written_val[which_buffer] = old_val` for host_trap method before
PM4 submission (amd_gpu_agent.cpp).

---

## Why Sample Retention Is Low

### How normal PC sampling works (GFX9/12)

On supported architectures, the **GPU hardware** generates samples. SPI programs SQ to
raise a trap every N instructions/cycles (interval 65536–1048576). When a trap fires,
**one wave** generates **one 64-byte sample**. The GPU trap handler assembly code
(`trap_handler.s` / `trap_handler_gfx12.s`) runs on the CU and writes the sample into
the device buffer, atomically incrementing `buf_written_val`. When the buffer hits the
80% watermark, the GPU decrements `done_sig`, which wakes the ROCr `PcSamplingThread`.

The sample rate is **inherently throttled by the interval**. With 304 CUs at interval
1M instructions, you get a few hundred to a few thousand samples per second. The
reference CSV in the docs has 78 samples from a short kernel.

### The buffer pipeline (sizes from code)

```
Stage                          Size         Record    Capacity
─────────────────────────────────────────────────────────────
Device buffer (trap_buffer)    2 MB         64 B      32,768 samples
Host buffer (ROCr)             8 MB         64 B      131,072 samples
HandleSampleData threshold     4 MB         64 B      65,536 samples (= session.buffer_size())
SDK buffer (tool.cpp)          64 KB        88 B      744 records
```

Sources:
- HSA buffer: `get_hsa_pcs_buffer_size()` = `64 * 1024 * 64` = 4 MB (utils.hpp:61)
- Device buffer: `trap_buffer_size = session.buffer_size() / 2` = 2 MB (amd_gpu_agent.cpp:2894)
- Host buffer: `2 * session.buffer_size()` = 8 MB (amd_gpu_agent.cpp:2895)
- SDK buffer: `16 * page_size` = 64 KB (tool.cpp:1936)
- SDK record: `rocprofiler_pc_sampling_record_host_trap_v0_t` = 88 bytes (pc_sampling.h:270)

### On GFX9/12, the pipeline works because:

1. Samples **trickle in** at the hardware-controlled rate
2. Device buffer **signals watermark** via `done_sig` → wakes PcSamplingThread
3. PcSamplingThread copies device→host via PM4 DMA, then calls `HandleSampleData`
   when host buffer accumulates ≥ `session.buffer_size()` (4 MB = 65K samples)
4. SDK buffer (744 records) **drains incrementally** via watermark flush callback
5. `LOSSLESS` policy is safe — emplace blocks if buffer full, buffer flushes via
   thread pool, emplace retries. No deadlock because the thread pool is independent
   and the mutex isn't involved in the flush path during normal operation.

### Our approach has three compounding problems:

**Problem 1: HandleSampleData is never called during execution.**
`PcSamplingThread` (amd_gpu_agent.cpp:3688) only calls `HandleSampleData` when:
```cpp
while (bytes_before_wrap >= session.buffer_size())  // 4 MB threshold
```
Our kernel delivers ~20K samples total = 1.25 MB. This **never reaches** the 4 MB
threshold. So `HandleSampleData` is never called during the thread loop. All data
sits in the host buffer until shutdown.

**Problem 2: Burst delivery at shutdown.**
`PcSamplingFlush` (amd_gpu_agent.cpp:3780) uses a different threshold:
```cpp
while (bytes_before_wrap > 0)  // drains everything
```
At shutdown, this delivers all ~20K samples to the SDK callback in a single burst.

**Problem 3: SDK buffer overflow with DISCARD.**
The SDK buffer is 64 KB = 744 records. When ~20K samples arrive in a burst, the
first ~744 fill the buffer. With `DISCARD` policy, the remaining ~19,256 are silently
dropped ("buffer too small (size=0)" errors). Result: **3.5% retention**.

With the original `LOSSLESS` policy this would deadlock instead of drop: emplace
blocks → buffer flush submits to thread pool → join() blocks → PcSamplingThread
holds mutex → finalization needs mutex → deadlock (Bug #1 above).

### Summary of architectural differences

```
                        Normal (GFX9/12)              Our approach (GFX11.5.1)
────────────────────────────────────────────────────────────────────────────────
Who generates samples   GPU hardware trap per wave    CPU kernel thread scans all waves
Samples per event       1 (one wave trapped)          ~900 (all active waves scanned)
Rate control            Hardware interval (65K–1M)    Scan interval (5ms, all waves)
Device buffer signal    GPU decrements done_sig       No signaling (CPU writes)
buf_written_val         GPU trap handler increments   Never set (had to pre-set it)
HandleSampleData        4 MB threshold reached        Eager drain (any data, fixed)
Delivery pattern        Steady trickle over lifetime  Incremental via eager drain (fixed)
```

---

## Next Steps

### ~~Step 1: Fix Incremental Delivery in PcSamplingThread~~ ✓ DONE

Fixed in amd_gpu_agent.cpp. For `host_trap` method, PcSamplingThread now uses eager drain
(`while (bytes > 0)` with `min(bytes, session.buffer_size())` chunks) instead of waiting
for the 4 MB threshold. The stochastic/GPU-trap path is unchanged.

### ~~Step 2: Revert DISCARD → LOSSLESS~~ ✓ DONE

Reverted tool.cpp buffer policy back to `ROCPROFILER_BUFFER_POLICY_LOSSLESS`.
With incremental delivery, the SDK buffer never overflows, so LOSSLESS doesn't block.

### ~~Step 3: Verify Sample Counts End-to-End~~ ✓ DONE

Verified with PCS_KERNEL_LAUNCHES=2000:
- Kernel: 20,526 delivered → CSV: 20,526 samples (100% retention)
- Zero "buffer too small" errors
- Clean shutdown, no deadlocks

### Step 4: Production Cleanup
- Remove debug fprintf statements from:
  - amd_gpu_agent.cpp (PcSamplingThread, PcSamplingFlush, PcSamplingStop, FlushDeviceBuffers)
  - registration.cpp (finalize steps)
  - tool.cpp (tool_fini, rocprofv3_main)
  - code_object.cpp (finalize)
  - hsa_adapter.cpp (flush_internal_agent_buffers)
- Reduce kernel logging (`pr_warn` → `pr_debug`)
- Remove dead code

### Step 5: Verify with Larger Workloads
- Test with PCS_KERNEL_LAUNCHES=200000 (longer GPU active time)
- Verify sample distribution across instructions
- Test with multiple concurrent workloads

---

## Architecture: Direct PC Reading

Read PC_LO/PC_HI from running waves via SQ_IND. No trapping, no SQ_CMD.

### Wave Iteration
```
for se in 0..num_se:
  for sh in 0..num_sh:
    for wgp in 0..max_cu_per_sh/2:
      for simd in 0..3:
        GRBM_GFX_INDEX: INSTANCE = (wgp << 2) | simd
        for wave in 0..15:
          STATUS → filter VALID, skip PRIV, skip corruption (bits 30-31)
          HW_ID2 → filter by VMID, skip 0xbebebeef
          PC_LO/HI → skip if either == 0xbebebeef
          → fill kfd_pcs_sample: pc, hw_id=HW_ID2, timestamp=ktime_get_raw_ns()
```

### Thread Lifecycle (critical for correctness)
```
Init:
  kfd_lookup_process_by_pasid → get_task_struct(lead_thread) → kfd_unref_process
  get_task_mm → kthread_use_mm → read TMA[0], buf_size → kthread_unuse_mm → mmput
  (NO long-lived mm or process ref — prevents circular dependency)

Loop:
  read_wave_pcs(sample_buf) → if samples > 0:
    get_task_mm(lead_thread) → if NULL: process died, break
    kthread_use_mm → pcs_write_to_device_data → kthread_unuse_mm → mmput

Exit triggers:
  - kthread_should_stop() — normal stop via kfd_pc_sample_stop ioctl
  - get_task_mm returns NULL — process exited (exit_mm set task->mm = NULL)
  - amdgpu_in_reset — GPU reset
```

### Why Per-Iteration MM Acquire/Release is Required
Holding `mm_users` across iterations prevents the ONLY KFD cleanup path:
```
Thread holds mm_users → exit_mmap blocked → mmu_notifier_release blocked →
kfd_process_notifier_release never fires → kfd_process_wq_release never runs →
kfd_process_destroy_pdds never runs → kfd_pc_sample_release never called →
kthread_stop never called → DEADLOCK
```
Fix: acquire/release mm per-iteration. When process exits, `get_task_mm` returns NULL.

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

### Shutdown Sequence (corrected)
```
registration::finalize:
  async_copy_fini
  counters_finalize
  queue_controller_fini
  thread_trace_finalize
  ompt_finalize
  kfd_finalize
  pc_sampling::stop_sampling_threads()    ← NEW: stops PcSamplingThread
    → PcSamplingStop → session.stop(), hsaKmtPcSamplingStop, signal -1, WaitForThread
    → flush_internal_agent_buffers        ← flushes remaining samples
  pc_sampling::code_object::finalize()    ← flushes again (no-op, thread stopped)
  pc_sampling::service_fini()             ← flushes again (no-op)
  code_object::finalize()
  invoke_client_finalizers()              ← tool_fini → stop_context (PcSamplingStop no-op) → CSV
  internal_threading::finalize()
```

## Known Limitations
- **Race with ROCr flush** — kernel CPU writes vs ROCr PM4 GPU swaps on `buf_write_val`.
  Benign for statistical sampling (may lose a few samples at swap boundaries).
- **Single-XCC only** — `for_each_inst` loop overwrites `n_samples`. Fine for Strix Halo.
- **`trigger_pc_sample_trap` is a no-op** — calls `read_wave_pcs(NULL, 0)`, returns 0.
- **Dispatch_Id / Correlation_Id / Exec_Mask = 0** — expected for direct-read approach
  (exec_mask can't be read reliably via SQ_IND, CID not assigned, dispatch not tracked).

## VMEM Hang in PRIV=1 (for future reference)

Most likely a **TMA memory mapping issue**, not a hardware bug.
CWSR handler's `s_load_dword` + `global_store_dword_addtid` work in PRIV mode on GFX11
(same `cwsr_trap_gfx11_hex` binary for all GFX11.x). Our TMA is separately allocated
and likely lacks valid GPU PTEs for data access (instruction fetch works at same VA range).

## Key Constraints

### GFX11 SQ_IND — No FORCE_READ
- Control registers (STATUS, HW_ID, PC) read reliably
- TTMP/SGPR reads return zero ~80% of time — unusable
- GRBM_GFX_INDEX INSTANCE = `(wgp << 2) | simd` (per-instance, not broadcast for reads)

### GFX11 ISA
- No SMEM stores (`s_store_*`, `s_atomic_*`, `s_dcache_wb`)
- `s_sleep` max 127; `HW_REG_HW_ID` → `HW_REG_HW_ID1`

## Key Files
- `~/amdgpu/.../amdgpu_amdkfd_gfx_v11.c` — `read_wave_pcs`, wave iteration
- `~/amdgpu/.../kfd_pc_sampling.c` — sampling thread, delivery, session management
- `~/amdgpu/.../kfd_priv.h` — `kfd_dev_pcs_hosttrap`
- `~/amdgpu/.../include/kgd_kfd_interface.h` — `kfd_pcs_sample` struct, `read_wave_pcs` callback
- `projects/rocr-runtime/.../amd_gpu_agent.cpp` — device_data alloc, TMA setup, flush logic
- `projects/rocr-runtime/.../core/inc/amd_gpu_agent.h` — `pcs_sampling_data_t` struct
- `projects/rocr-runtime/.../inc/hsa_ven_amd_pc_sampling.h` — `perf_sample_hosttrap_v1_t`
- `projects/rocr-runtime/.../pcs/pcs_runtime.h` — `PcSamplingSession`, `buffer_size()`, `sample_size()`
- `projects/rocprofiler-sdk/.../tool.cpp` — SDK buffer size (line 1936), buffer policy (line 1777)
- `projects/rocprofiler-sdk/.../pc_sampling/utils.hpp` — HSA buffer size (4 MB)
- `projects/rocprofiler-sdk/.../pc_sampling/service.cpp` — stop_sampling_threads()
- `projects/rocprofiler-sdk/.../registration.cpp` — finalization order
- `projects/rocprofiler-sdk/.../buffer.hpp` — LOSSLESS/DISCARD emplace logic (line 171)
- `projects/rocr-runtime/.../trap_handler/trap_handler.s` — GPU trap handler (GFX9/9.4)
- `projects/rocr-runtime/.../trap_handler/trap_handler_gfx12.s` — GPU trap handler (GFX12)

## Build & Test
```bash
# Kernel (requires sudo + reboot — ask user):
cd ~/amdgpu && sudo dkms build amdgpu/1.0 && sudo dkms install amdgpu/1.0 --force
# ROCr (no reboot):
cd ~/rocm-systems && bash build_rocr.sh
# Rocprofiler-SDK (no reboot):
cd ~/rocm-systems && bash build_rocprofiler.sh
# Test:
PCS_TIMEOUT_SEC=30 PCS_KERNEL_LAUNCHES=2000 bash test_pc_sampling.sh
# Check kernel logs:
dmesg | grep -E "pcs|read_pcs|delivery"
```
