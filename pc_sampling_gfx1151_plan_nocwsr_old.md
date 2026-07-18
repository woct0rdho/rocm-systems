# PC Sampling on gfx1151 (Strix Halo) with CWSR Disabled (Outdated)

> **Historical document only.** The final branch uses the CWSR daisy-chain trap-handler architecture described in `pc_sampling_gfx1151_plan_hosttrap.md` and `pc_sampling_gfx1151_plan_stochastic.md`. Do not restore this direct `SQ_IND` scanning design during future rebases.

## Platform

- RDNA 3.5 APU (GFX11.5.1), shared memory
- 2 SEs, 2 SHs/SE, 40 CUs (20 WGPs), 4 SIMDs/WGP, max_waves_per_simd=16, wave32
- Linux 6.19, mainline amdgpu driver converted to DKMS module

## Architecture: Direct PC Reading

Read `PC_LO`/`PC_HI` from running waves via `SQ_IND`. No trapping, no `SQ_CMD`.

### How It Differs from Normal PC Sampling (GFX9/12 in non-mainline driver)

On GFX9/12, the **GPU hardware** traps one wave every N instructions/cycles. The trap
handler assembly writes one 64-byte sample into the device buffer and increments
`buf_written_val`. At the 80% watermark, GPU decrements `done_sig` to wake the ROCr
`PcSamplingThread`. Samples trickle in at hardware-controlled rate.

On GFX11.5, the **kernel thread** scans all running waves at a user-specified
interval via `SQ_IND`, producing ~900 samples per scan. The kernel writes directly
to the device buffer from CPU via `kthread_use_mm`. There is no GPU signaling --
`buf_written_val` is never set by the GPU (ROCr pre-sets it to satisfy PM4
`WAIT_REG_MEM`). The ROCr `PcSamplingThread` eager-drains any available data instead
of waiting for a 4 MB threshold.

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
  (NO long-lived mm or process ref -- prevents circular dependency)
  If delivery init fails -> goto exit_cleanup -> exit_wait (no useless spinning)

Loop:
  read_wave_pcs(sample_buf) -> if samples > 0:
    get_task_mm(lead_thread) -> if NULL: process died, break
    kthread_use_mm -> pcs_write_to_device_data -> kthread_unuse_mm -> mmput
    if pcs_write_to_device_data returns -EFAULT: buffer gone, break

Exit triggers:
  - kthread_should_stop() -- normal stop via kfd_pc_sample_stop ioctl
  - get_task_mm returns NULL -- process exited
  - pcs_write_to_device_data returns -EFAULT -- device buffer unmapped
  - amdgpu_in_reset -- GPU reset
  - SIGKILL

All exit paths (including early init failures) converge to exit_wait:
  while (!kthread_should_stop())
      schedule_timeout_uninterruptible(100ms);
This guarantees the task_struct stays valid for kthread_stop() in the stop path.
The stop path clears pc_sample_thread after kthread_stop() returns.
```

### TMA Indirection (CWSR two-level trap handler)

When CWSR is active, `qpd->tma_addr` points to the first-level (CWSR) TMA.
ROCr's `SetTrapHandler` writes the second-level TBA/TMA into the CWSR TMA page
via `kfd_process_set_trap_handler`:

```
CWSR TMA (kernel-accessible via cwsr_kaddr + KFD_CWSR_TMA_OFFSET):
  [0] = second-level TBA (trap handler code GPU VA)
  [1] = second-level TMA (ROCr's coarsegrain TMA region)

Second-level TMA (userspace, read via kthread_use_mm + get_user):
  [0] = hosttrap device_data GPU VA
  [1] = stochastic device_data GPU VA
```

`kfd_pc_sample_start` reads `cwsr_tma[1]` from kernel memory to get the
second-level TMA address, which the kernel thread then dereferences via
`get_user` to find the device_data VA at offset 0.

### Why Per-Iteration MM Acquire/Release is Required

Holding `mm_users` across iterations prevents the **only** KFD cleanup path:
```
Thread holds mm_users -> exit_mmap blocked -> mmu_notifier_release blocked ->
kfd_process_notifier_release never fires -> kfd_process_wq_release never runs ->
kfd_process_destroy_pdds never runs -> kfd_pc_sample_release never called ->
kthread_stop never called -> DEADLOCK
```

### Start Rollback

If `kfd_pc_sample_thread_start` fails (`kthread_run` error or thread init failure),
`kfd_pc_sample_start` rolls back: resets `pcs_entry->enabled`, decrements
`active_count`, and clears hosttrap state. Userspace gets a clean error.

### ROCr Buffer Pipeline

```
Stage                          Size         Record    Capacity
--------------------------------------------------------------
Device buffer (trap_buffer)    2 MB         64 B      32,768 samples
Host buffer (ROCr)             8 MB         64 B      131,072 samples
HandleSampleData threshold     4 MB / any   64 B      65K / 1 sample (stochastic / host_trap)
SDK buffer (tool.cpp)          64 KB        88 B      744 records
```

For `host_trap`, `PcSamplingThread` uses eager drain (`while (bytes > 0)` with
`min(bytes, session.buffer_size())` chunks). Buffer policy is **LOSSLESS**.

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

## Userspace Bugs Fixed

### 1. PM4 `WAIT_REG_MEM` Hang (ROCr)

PM4 `WAIT_REG_MEM` polls `buf_written_val` to equal `old_val`. GPU trap handler
normally increments it, but kernel direct-read never does. Fix: pre-set
`buf_written_val[which_buffer] = old_val` for `host_trap` before PM4 submission.

### 2. Finalization Order Deadlock (ROCProfiler)

`code_object::finalize()` needs `host_buffer_mutex`, but `PcSamplingThread` holds it.
Original order called `code_object::finalize()` before stopping the thread. Fix: added
`pc_sampling::stop_sampling_threads()` in `registration.cpp`, called before
`code_object::finalize()`.

### 3. LOSSLESS Buffer Deadlock / Low Retention (ROCProfiler + ROCr)

`PcSamplingThread` only called `HandleSampleData` when host buffer reached 4 MB. Our
~20K samples (1.25 MB) never hit that threshold. All data sat until shutdown flush,
then burst-delivered to the 744-record SDK buffer. With **LOSSLESS**, emplace blocked
and deadlocked. With **DISCARD**, 96.5% of samples were dropped.

Fix: For `host_trap`, `PcSamplingThread` now eager-drains (`while (bytes > 0)`) instead
of batching to 4 MB. With incremental flow, **LOSSLESS** works without deadlock.

## Known Limitations

- **Race with ROCr flush** -- kernel CPU writes vs ROCr PM4 GPU swaps on `buf_write_val`.
  Benign for statistical sampling (may lose a few samples at swap boundaries).
- **Single-XCC only** -- `for_each_inst` loop overwrites `n_samples`. Fine for Strix Halo.
- **Dispatch_Id / Correlation_Id / Exec_Mask = 0** -- expected for direct-read approach.
- **~20% sentinel corruption** -- `0xbebebeef` in STATUS bits 30-31, `HW_ID2`, or PC.
  Filtered in `read_wave_pcs`.
- **`grbm_idx_mutex` held for entire wave scan** (~12,800 MMIO reads on Strix Halo).
  Blocks concurrent GRBM-indexed operations for the scan duration.
- **Device buffer full**: samples silently dropped (no backpressure). ROCr eager-drain
  keeps the buffer mostly empty in practice.
- **VMID lookup** via `IH_VMID_0_LUT` runs every iteration when VMID=0 (~16 MMIO reads).
  Resolves quickly once `allocate_vmid` assigns the VMID.
- **`remap_queue` MES path**: if `add_all_kfd_queues_mes` fails after `remove_all`,
  queues are lost. No recovery attempted.

## Hardware Constraints

### GFX11 `SQ_IND` -- No FORCE_READ

- Control registers (`STATUS`, `HW_ID`, `PC`) read reliably
- TTMP/SGPR reads return zero ~80% of time -- unusable
- `GRBM_GFX_INDEX` INSTANCE = `(wgp << 2) | simd`

### GFX11 ISA

- No SMEM stores (`s_store_*`, `s_atomic_*`, `s_dcache_wb`)
- `s_sleep` max 127; `HW_REG_HW_ID` -> `HW_REG_HW_ID1`

## Sampling Rate and Quality

### Interval Parameter

The kernel thread uses the user-specified interval (microseconds for `host_trap`).
Validated at create time: `interval_min`=1 us, `interval_max`=`UINT_MAX` us (~4295 s).
Intervals > `UINT_MAX` are rejected to prevent silent u64->u32 truncation.

`rocprofv3` requires `--pc-sampling-interval` (mandatory, no default). The
internal SDK fallback is 1 (`config.hpp:141`). AMD's ROCm Compute Profiler docs
recommend starting at 1048576 us (~1s) and lowering to 65536 us (~65ms).

### Throughput per Interval

```
GFX9/12 (trigger_pc_sample_trap):
  SQ_CMD traps 1 random wave -> trap handler writes 1 sample

GFX11.5 (read_wave_pcs):
  SQ_IND scans ALL active waves -> kernel writes ~N samples (N = active waves)
```

At typical occupancy (~900 active waves on Strix Halo):
```
Interval     GFX9/12 host-trap    GFX11.5 direct-read
5 ms         ~200 samples/sec     ~180K samples/sec
65 ms        ~15 samples/sec      ~14K samples/sec
1 s          ~1 sample/sec        ~900 samples/sec
```

### Sample Quality

```
Field             GFX9/12 host-trap       GFX11.5 direct-read    Notes
pc                TTMP0/TTMP1 (HW saved)  SQ_IND PC_LO/PC_HI     Equivalent
exec_mask         EXEC register           0                      SQ_IND can't read EXEC on gfx11
workgroup_id      TTMP8/9/10 (SPI)        0                      TTMP reads return garbage via SQ_IND
hw_id             HW_REG_HW_ID            SQ_IND HW_ID2          Equivalent (wave/simd/wgp/sa/se)
timestamp         SQ timestamp counter    ktime_get_raw_ns()     CPU vs GPU clock; fine for statistics
correlation_id    0 (SDK parser resolves) 0                      Both rely on SDK dispatch tracking
chiplet_info      TTMP11 bits             0                      Single-chiplet on Strix Halo
```

For the primary use case (instruction-level hotspot profiling), `pc` + `hw_id` +
`timestamp` is sufficient.

## Key Files

### Mainline Driver (`~/amdgpu-mainline/`)

- `.../amdgpu_amdkfd_gfx_v11.c` -- `read_wave_pcs`, `program_trap_handler_settings_v11`,
  `get_atc_vmid_pasid_mapping_info_v11`
- `.../kfd_pc_sampling.c` -- sampling thread, `pcs_write_to_device_data`, VMID lookup,
  session lifecycle, start rollback
- `.../kfd_priv.h` -- `kfd_dev_pcs_hosttrap` fields
- `.../include/kgd_kfd_interface.h` -- `kfd_pcs_sample` struct, `read_wave_pcs` callback
- `.../kfd_device_queue_manager.c` -- `allocate_vmid` target_vmid refresh, `remap_queue`
- `.../kfd_chardev.c` -- ioctl handler
- `.../kfd_process.c` -- `kfd_pc_sample_release` in teardown
- `.../kfd_device.c` -- `pcs_data` mutex/idr init and cleanup

### ROCr (`projects/rocr-runtime/`)

- `.../amd_gpu_agent.cpp` -- device_data alloc (`system_allocator` + `MakeMemoryResident`),
  TMA setup (`coarsegrain` + GPU VA), `PcSamplingThread` (5ms timeout for `host_trap`,
  eager drain), flush (`buf_written_val` pre-set)
- `.../core/inc/amd_gpu_agent.h` -- `pcs_sampling_data_t` struct
- `.../trap_handler/trap_handler.s` -- GFX11 host trap entry, TMA decode, sleep loop

### ROCProfiler (`projects/rocprofiler-sdk/`)

- `.../tool.cpp` -- SDK buffer size, buffer policy (**LOSSLESS**)
- `.../pc_sampling/service.cpp` -- `stop_sampling_threads()`
- `.../registration.cpp` -- finalization order
- `.../pc_sampling/parser/translation.hpp` -- GFX11 `HW_ID2` parsing

## Mainline Kernel Port (`~/amdgpu-mainline/`)

Ported from the ROCm `pc_sampling_gfx1151` branch to mainline 6.19 kernel tree,
built as a DKMS module. GFX11.5 hosttrap only (no stochastic, no GFX9/12 fallback).

### Files Modified

- `include/uapi/linux/kfd_ioctl.h` -- PC sampling ioctl at 0x85, second command
  range (0x80..0x87)
- `drivers/gpu/drm/amd/include/kgd_kfd_interface.h` -- `kfd_pcs_sample` struct,
  `read_wave_pcs` callback
- `drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v11.c` -- `get_atc_vmid_pasid_mapping_info_v11`,
  `program_trap_handler_settings_v11`, `kgd_gfx_v11_read_wave_pcs`
- `drivers/gpu/drm/amd/amdkfd/kfd_priv.h` -- data structures, DKMS uapi guards,
  second ioctl range definitions
- `drivers/gpu/drm/amd/amdkfd/kfd_topology.c` -- MES firmware version check fix
- `drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c` -- `allocate_vmid` refresh,
  `set_pasid_vmid_mapping` ret init, `remap_queue`
- `drivers/gpu/drm/amd/amdkfd/kfd_chardev.c` -- ioctl handler, second range dispatch
- `drivers/gpu/drm/amd/amdkfd/kfd_process.c` -- `kfd_pc_sample_release` call
- `drivers/gpu/drm/amd/amdkfd/kfd_device.c` -- `pcs_data` init/cleanup
- `drivers/gpu/drm/amd/amdkfd/Makefile` -- `kfd_pc_sampling.o`

### Files Created

- `drivers/gpu/drm/amd/amdkfd/kfd_pc_sampling.h`
- `drivers/gpu/drm/amd/amdkfd/kfd_pc_sampling.c`

### Key Differences from ROCm Version

- GFX11.5 hosttrap only -- no stochastic, no GFX9/12 `trigger_pc_sample_trap`
- No `kfd_dbg_enable_ttmp_setup` (not in mainline)
- No `kfd_process_set_trap_pc_sampling_flag` (CWSR handles trap setup normally)
- DKMS uapi guard (`#ifndef KFD_IOCTL_PCS_FLAG_POWER_OF_2`) in `kfd_priv.h`
- Ioctl at 0x85 with second command range (0x80..0x87), matching ROCm convention
- MES firmware version check relaxed for mainline firmware (`mes_api_rev == 0`)
- CWSR two-level TMA indirection handled in `kfd_pc_sample_start`

## DKMS Integration Issues Fixed

### 1. MES Firmware Version Check (`kfd_topology.c`)

Mainline MES firmware reports `mes_api_rev=0` (doesn't encode API version in upper
bits like ROCm firmware). The original check `(mes_api_rev >= 14) && (mes_rev >= 64)`
failed, so `HSA_CAP_TRAP_DEBUG_FIRMWARE_SUPPORTED` was never set. This blocked the
entire PC sampling path in ROCr (`supports_exception_debugging == false`).

The capability check chain:
```
kfd_topology_set_dbg_firmware_support (kernel)
  -> HSA_CAP_TRAP_DEBUG_FIRMWARE_SUPPORTED bit in capability
  -> hsaKmtCheckRuntimeDebugSupportCtx (thunk): checks DebugSupportedFirmware
  -> hsaKmtRuntimeEnable returns NOT_SUPPORTED
  -> KfdDriver::Init sets supports_exception_debugging = false
  -> GpuAgent::PcSamplingIterateConfig bails out
  -> rocprofiler gets empty config list
  -> "Given PC sampling configuration is not supported on any of the agents"
```

Fix: accept `mes_api_rev == 0` when `mes_rev >= 64`. Strix Halo has mes_rev=128.

### 2. Ioctl Number Mismatch (`kfd_priv.h`, `kfd_chardev.c`, `kfd_ioctl.h`)

The initial port placed `AMDKFD_IOC_PC_SAMPLE` at ioctl 0x27 (appended to the main
range). The ROCm driver and rocprofiler-sdk use 0x85 in a second ioctl range
(0x80..0x87). Additionally, the DKMS build uses system kernel headers where
`AMDKFD_COMMAND_END = 0x27`, so even the 0x27 placement was excluded from dispatch
(`nr < AMDKFD_COMMAND_END` failed -> ENOTTY).

The rocprofiler capability check chain:
```
ioctl_adapter.cpp: get_pc_sampling_ioctl_version
  -> ioctl(fd, AMDKFD_IOC_PC_SAMPLE, ...) with op=QUERY_CAPABILITIES
  -> kernel: _IOC_NR(cmd) = 0x85
  -> kfd_ioctl dispatch: nr checked against COMMAND_START_2..COMMAND_END_2
  -> kfd_pc_sample_query_cap: returns supported methods + PCS ioctl version
```

Fix: moved ioctl to 0x85, added `AMDKFD_COMMAND_START_2`/`END_2` definitions in
`kfd_priv.h` (with `#ifndef` guard), and added second range check in `kfd_ioctl()`.

### 3. CWSR TMA Indirection (`kfd_pc_sampling.c`)

`kfd_pc_sample_start` stored `pdd->qpd.tma_addr` as `trap_tma_addr`. With CWSR
active, this is the first-level TMA. The kernel thread read `TMA[0]` expecting the
device_data VA but got the second-level TBA (trap code address) instead, causing
delivery init to fail ("pcs: delivery init failed, thread not starting").

Fix: when `cwsr_kaddr` is set, read `cwsr_tma[1]` (the second-level TMA address)
from kernel memory and store that as `trap_tma_addr`. Pending DKMS rebuild.

## Bugs Fixed During Code Review

1. **Use-after-free in `kfd_pc_sample_stop`** -- thread cleared `pc_sample_thread`
   before exiting; stop path could call `kthread_stop` on freed `task_struct`.
   Fix: thread waits in `kthread_should_stop()` loop; stop path clears pointer
   after `kthread_stop`.
2. **Early thread exits bypassed `kthread_should_stop` wait** -- `!timeout` and
   `!have_delivery` paths returned directly. Fix: all paths goto
   `exit_cleanup`/`exit_wait`.
3. **Thread ran uselessly when delivery init failed** -- loop spun without reading
   waves or delivering samples. Fix: goto `exit_cleanup` on `!have_delivery`.
4. **SIGKILL caused busy-loop in exit wait** -- `schedule_timeout_interruptible`
   returned immediately with pending signal. Fix: `schedule_timeout_uninterruptible`.
5. **No rollback on thread start failure** -- `enabled`/`active_count` left
   inconsistent. Fix: `kfd_pc_sample_start` rolls back state on failure.
6. **`pcs_write_to_device_data` return ignored** -- `-EFAULT` caused infinite retry
   loop. Fix: `-EFAULT` breaks the thread loop.
7. **`__user` annotation on kernel pointer** -- `kfd_pc_sample`/`query_cap`/`create`
   took `__user` on args that are kernel-copied by ioctl infrastructure. Fix: removed.
8. **Interval u64->u32 truncation** -- intervals > `UINT_MAX` silently wrapped.
   Fix: rejected at create time.

## Risks If Testing on Other GPUs

### MEDIUM RISK -- Kernel

- **`remap_queue` MES path** -- `remove_all` + `add_all` instead of no-op. Could cause
  brief queue interruption on GFX12 if it starts PC sampling via hosttrap.
- **`kfd_dbg_set_mes_debug_mode`** -- called for ALL MES GPUs during hosttrap start.
- **`target_vmid` instead of `last_vmid_kfd`** -- more correct but behavior change for
  GFX9/GFX12. If VMID resolution fails, trap is skipped entirely.
- **`program_trap_handler_settings` in thread loop** -- redundant but harmless
  re-programming for GFX9/GFX12. Acquires SRBM lock briefly.

### MEDIUM RISK -- ROCr

- **TMA allocation** -- `finegrain_allocator` -> `coarsegrain_allocator` +
  `MakeMemoryResident` + GPU VA. On dGPU, `MakeMemoryResident` may be a no-op; untested.
- **device_data allocation** -- `finegrain_allocator` -> `system_allocator` with
  0x1000 alignment + `MakeMemoryResident`.

## Build & Test

```sh
# Kernel (requires sudo + reboot):
sudo dkms build --force amdgpu/1.0 && sudo dkms install --force amdgpu/1.0
# ROCr (no reboot):
cd ~/rocm-systems && bash build_rocr.sh
# ROCProfiler (no reboot):
cd ~/rocm-systems && bash build_rocprofiler.sh
# Test:
PCS_TIMEOUT_SEC=30 PCS_KERNEL_LAUNCHES=2000 python test_pc_sampling.py
# Check kernel logs:
dmesg | grep -E "pcs:"
```

## Next Steps

- Verify sample distribution across instructions with larger workloads
- Test with multiple concurrent workloads
- If other-GPU testing shows regressions, condition kernel/ROCr changes on
  `IP_VERSION(11,5,x)`
