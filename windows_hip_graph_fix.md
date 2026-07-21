# Native Windows ROCr/CLR/WDDM graph submission failure

This note records the native Windows graph failure, the retained source fix, the build used to validate it, and a ROCr/HIP-only regression test. The test uses the HSA API exposed by `amdhip64_7.dll`; it has no model or application-specific dependency.

## Environment

- Repository: `C:\rocm-systems`
- Branch: `rocprofiler-windows`
- GPU: AMD Radeon(TM) 8060S, `gfx1151`
- ROCm package prefix: `C:\venv_torch\Lib\site-packages\_rocm_sdk_devel`
- Build type: `RelWithDebInfo`

## Failure

The native Windows run reported:

```text
WDDMDevice::SubmitToHwQueue fail c01e0200
ComputeQueue::AqlToPm4Thread process compute queue fail status = 00001000
```

`c01e0200` is `STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE`. The failure occurs while a graph batch is submitted through the Windows CLR device queue and consumed by the WDDM/ROCr path.

The working root-cause model is an ordering failure in graph metadata publication. A graph batch reserves a group of AQL slots, copies packet bodies with non-temporal stores, and publishes packet headers separately. The metadata prefetch ring contains four independently visible 64-byte segments per packet. The old non-`MOVDIR64B` path copied each complete metadata packet, including valid headers, without first keeping those headers invalid. A WDDM/ROCr consumer could therefore observe an armed metadata header while its body was still only partially visible, or while the slot still contained data from a previous use. The consumer then generated invalid PM4 work and the device exception above.

The device-resident queue work in `f79ca3d3a9` makes this ordering important on the queue path used by graph batches. Commit `0125f0ac18` also records the same required body-first/header-last ordering and the use of `HSA_PACKET_TYPE_INVALID` for metadata slots that must be skipped.

## Retained fix

The worktree retains these source changes:

### CLR metadata publication (now upstream)

`projects/clr/rocclr/device/rocm/rocvirtual.cpp` in the current base already publishes each metadata segment body-first with its header released afterwards, and its comments document the non-`MOVDIR64B` versus `MOVDIR64B` split. The branch-local copy of that fix was dropped during the rebase onto `origin/pc_sampling_gfx1151` so the file stays upstream's; metadata prefetch is not enabled on the queue path this machine exposes (see "Local metadata limitation"), so the ordering could not be exercised here in either revision.

### ROCr scratch recovery

`projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_aql_queue.h` and `projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp`:
- Change `HandleInsufficientScratch` from `void` to `bool` so failure to find a usable dispatch packet is propagated.
- Acquire-load packet headers while scanning for the dispatch that needs scratch.
- Yield and retry unpublished headers up to 64 times, then return failure instead of dereferencing a null packet.
- Convert dynamic queue exhaustion into a fatal generic HSA error.
- Treat a WDDM `error_code == -1` as `HSA_STATUS_ERROR`. It is a generic queue-failure sentinel, not the `0x401` insufficient-scratch bitmask.

This remains a branch change on top of the current base and is what the fixed-runtime regression below covers.

## Build and install

The ROCr runtime was rebuilt and installed with:

```pwsh
C:\rocm-systems\projects\rocr-runtime\scripts\build_windows.ps1 `
  -VenvPath C:\venv_torch `
  -BuildDirectory C:\rocm-systems\build\rocr-runtime `
  -InstallPrefix C:\venv_torch\Lib\site-packages\_rocm_sdk_devel `
  -BuildType RelWithDebInfo
```

The HIP DLL embeds a static ROCr runtime, so rebuild the configured CLR target after changing either ROCr or CLR sources:

```pwsh
cmake --build C:\rocm-systems\build\clr-hip `
  --config RelWithDebInfo `
  --target amdhip64
```

The fixed DLL used below was produced at:

```text
C:\rocm-systems\build\clr-hip\hipamd\src\amdhip64_7.dll
```

## Component validation

The maintained Windows ROCr component gate is:

```pwsh
ctest --test-dir C:\rocm-systems\build\rocr-runtime `
  -C RelWithDebInfo `
  -R "cmdbuf_frame_ring|rocr\.windows\.(queue-profiling|runtime-binary)" `
  --output-on-failure
```

The earlier `hsakmt.windows.packet-publication` and `hsakmt.windows.profiling-adapter` entries referenced by older revisions are no longer registered. `cmdbuf_frame_ring` now covers the frame-ring scheduling contract, and `rocr.windows.queue-profiling` covers the shared profiling property.

Also run:

```pwsh
git -C C:\rocm-systems diff --check
```

## Fixed-runtime regression test

The probe is stored in `rocr_scratch_recovery_test.cpp`. The build and run wrapper is `build_rocr_scratch_recovery_test.ps1`.

The probe enters the ROCr dynamic queue event handler used for scratch recovery. It creates a GPU HSA queue, sets `read_dispatch_id = 1` and `write_dispatch_id = 0` so there is no pending dispatch, and injects the `0x401` insufficient-scratch event through the queue inactive signal.

The current runtime intentionally waits for the write-index-ordered header of a pending packet. The probe must not advance the write index behind a permanently invalid header: that is a transient publication state, and `HandleInsufficientScratch()` would spin waiting for the header. With no pending dispatch, the handler treats the scratch event as fatal, suspends the queue, reports `HSA_STATUS_ERROR`, and completes the event with signal value `-1`.

### Build and run with the fix

Rebuild the ROCr runtime and the embedded HIP DLL using the commands above. Then run the wrapper from the repository root:

```pwsh
powershell -ExecutionPolicy Bypass -File C:\rocm-systems\build_rocr_scratch_recovery_test.ps1 -Run
```

The script compiles the probe against `amdhip64.lib`, stages the fixed DLL from `C:\rocm-systems\build\clr-hip\hipamd\src\amdhip64_7.dll` beside the executable, adds the package `bin` and `lib` directories to `PATH`, and runs the staged executable. The executable directory controls DLL selection, and the probe prints the loaded DLL path.

To use a different fixed build, pass its DLL explicitly:

```pwsh
powershell -ExecutionPolicy Bypass -File C:\rocm-systems\build_rocr_scratch_recovery_test.ps1 `
  -RuntimeDll C:\path\to\amdhip64_7.dll `
  -Run
```

Expected fixed-runtime output includes:

```text
amdhip64=C:\rocm-systems\build\rocr-scratch-recovery\fixed\amdhip64_7.dll
injecting error=0x401 with no pending dispatch at dispatch_id=1
queue_signal=-1 callback_status=4096 (HSA_STATUS_ERROR: A generic error has occurred.)
rocr_scratch_recovery=passed
```

The wrapper returns exit code `0` only when the queue signal reaches `-1`, the callback reports `HSA_STATUS_ERROR`, and the probe prints `rocr_scratch_recovery=passed`.

## Local metadata limitation

The local HSA queue query returned metadata prefetch version `255.255` and metadata ring address `0x0`. Metadata prefetch is therefore not enabled on the queue path available on this machine. The direct graph metadata race cannot be reproduced here. The fixed-runtime regression above is intentionally limited to the ROCr queue-error callback boundary.

For separate graph-path validation on a machine with metadata prefetch support, force the relevant non-`MOVDIR64B` and system-memory paths:

```pwsh
$env:DEBUG_CLR_USE_MOVDIR64B = '0'
$env:DEBUG_CLR_AQL_DEV_QUEUE = '0'
```

## Second failure: retained PM4 graph lowering on Windows

With `DEBUG_HIP_GRAPH_PM4=1` set in the environment, a model run fails while a graph batch is submitted:

```text
[hipGraph][PM4] retained 4826 dispatches in 139627 dwords, max private 0 bytes
[wsl::thunk::ComputeQueue::VendorSpecificAqlToPm4] reject AQL Profile vendor packet:
  validation status 3 format=1 manifest=00000000 ib_header=c0023f00
[wsl::thunk::ComputeQueue::AqlToPm4Thread] process compute queue fail status = 00001009
[wsl::thunk::ComputeQueue::HandleError] error 4105, sig_val 32
GPU HANG ANALYSIS ... Vendor packet (amd_format=1)
HSA_STATUS_ERROR_INVALID_PACKET_FORMAT: The AQL packet is malformed. code: 0x1009
```

`validation status 3` was our own thunk's `kInvalidManifest`. HIP lowered the graph to a retained PM4 command list (`hsa_ven_amd_graph_*`), and ROCR materialized it as an AQL vendor packet with `vendor_header = 1`, `dword_count_remaining = 10`, and a canonical `IT_INDIRECT_BUFFER` header, but left `reserved[0..7]` zero. That diagnostic was valid for the revision that carried the private packet-manifest validation; the validator is no longer in the tree (see below).

The current Windows thunk no longer stops at 1984 dwords. A stream that fits the remainder of the current frame is inlined there; a larger retained list is copied into a dedicated variable-size command buffer and submitted as the command buffer itself. WDDM only makes the submitted command buffer fetchable, so an indirect-buffer jump from a frame into a separate allocation is not an option. The stream cap is the 20-bit `IT_INDIRECT_BUFFER` length field (`0xfffff` dwords, about 4 MiB). A list larger than that still returns `HSA_STATUS_ERROR_OUT_OF_RESOURCES` from command-list creation so HIP falls back to the AQL batch path.

### Retained fix

Upstream owns the feature side: `hsa_ven_amd_graph_*` (opaque prepared command lists, the GFX11/GFX12 encoders, queue scratch binding), the HIP graph integration and its packet-batch merge, and the `DEBUG_HIP_GRAPH_PM4` switch all come from `origin/pc_sampling_gfx1151`. This port adds the Windows variable-size submission path:

- `projects/rocr-runtime/runtime/hsa-runtime/core/runtime/hsa_ven_amd_graph.cpp`
  - A version-2 capability permits a command list up to `profiling::kMaximumIbDwords`; a version-1 thunk keeps the old `MaxPm4Dwords` limit. A list beyond the effective limit is rejected with `HSA_STATUS_ERROR_OUT_OF_RESOURCES`, so HIP falls back to the AQL batch path.
- `projects/rocr-runtime/libhsakmt/include/impl/wddm/profiling.h`
  - Owns the capability (`DetectCapability`, `kQualifiedFrameBytes`, `kMaximumPm4Dwords`, and `kMaximumIbDwords`). `MaxPm4Dwords` remains the inline/profiling budget; it is not the graph command-list cap for a version-2 thunk.
- `projects/rocr-runtime/libhsakmt/include/impl/wddm/queue.h` and `queue.cpp`
  - Add a two-slot `LargeCommandBufferState` behind a pointer. A large vendor packet flushes the pending frame, copies the PM4 stream with its optional leading AQL barrier and completion trailer into a `Wkmi::kSystem` command buffer, and submits that buffer as one command buffer. A slot is reused only after the monitored fence of the submission that consumed it has retired. The queue submits the same `NextFenceValue(sync_point)` fence as the inline path and does not use fixed sleeps or deadlines.

The runtime packet manifest (`WRTM`) and the thunk-side `WRTM`/`WCMP` validation remain dropped. The current path trusts the producer packet and validates only size, bounds, and the command-list cap; there is no checksum on the hot path.

Software-queue behaviors worth recording because they are specific to this port:

- On this KMD (`aql_queue=0`), ordinary execution uses the software AQL-to-PM4 queue. `SwitchAql2PM4()` uses the frame-derived merge limit rather than forcing one packet per frame. The installed PMC suite and the configured 16-test SDK graph pass on the current tree.
- `VendorSpecificAqlToPm4()` emits a `CS_PARTIAL_FLUSH` wait when the vendor packet's AQL header carries the barrier bit. Native AQL submission gets that ordering from hardware; the software translation must write it, and without it a counter read packet runs before the dispatch it samples has finished and every counter reads back zero.
- Signal retirement in the software queue waits for the GPU-written `ring_rptr` after the dispatch barrier and cache flush, then decrements the completion signal. There is no fixed 50 ms delay and no 30 s deadline in the hot path. Device exceptions are delivered through the ROCr queue-inactive signal; a `UINT64_MAX` progress fence is not treated as device loss because live queues can expose it on this stack.
- Frame reuse waits on the KMD monitored fence. Scratch growth drains the submission fence before freeing the old scratch allocation; without that drain, earlier in-flight submissions could page-fault on the recycled allocation, which was the pp512 failure.
- An oversized vendor payload returns `HSA_STATUS_ERROR_OUT_OF_RESOURCES` and leaves the AQL packet unretired. It is no longer dropped while completion is signaled. The large-buffer path accepts up to `kMaximumIbDwords`; a list beyond that is rejected before submission.
- Capability detection requires the effective software queue instead of hard-coding `compute_submission_supported`. The `hsaKmtGetWddmAqlProfileCapability` export is optional so older thunks still load; the graph path falls back when it is absent. Version 2 advertises the variable-size path; version 1 remains limited to the inline frame budget.
- The profiler completion barrier precedes the application's completion barrier. The completion waiter sleeps on a condition variable when idle, is stopped and joined at finalization, and `Queue::sync()` has no fixed 5 s deadline. The tool finalizer waits on a record event instead of a 2 s drain.
- Equal start/end timestamps are accepted for zero-duration kernels; only reversed intervals are rejected. The scratch publication path waits for the write-index-ordered header instead of failing after 64 yields.
- The command-buffer budget is 8 MiB. The frame includes the PMC worst-case headroom, and the larger budget keeps the frame ring deep enough that a long prompt batch does not exhaust it.

### Verification

The maintained gates all passed on this source and installed DLL:

```text
ROCr:  cmdbuf_frame_ring, rocr.windows.queue-profiling, rocr.windows.runtime-binary  3/3
CLR:   clr.windows.runtime-binary, registration.normal, registration.late          3/3
SDK:   configured CTest graph                                                      16/16
```

`rocprofiler-sdk-windows-hip-workload.exe --graph --dispatches 8` passed validation with both arms of the retained-list switch:

```text
DEBUG_HIP_GRAPH_PM4=1: dispatches=8 execution=graph validation=passed
DEBUG_HIP_GRAPH_PM4=0: dispatches=8 execution=graph validation=passed
```

A temporary chain harness exercised the variable path directly: 600 dispatches with five replays and 4096 dispatches with two replays both produced the expected final value with `DEBUG_HIP_GRAPH_PM4=1`; the 600-dispatch case also passed with PM4 disabled. The real Qwen3.8-Flash-Next IQ4_NL graph retained 2128 dispatches in 58451 dwords during the diagnostic run.

Two interleaved `llama-bench -p 512 -n 128 -r 3` rounds on Qwen3.8-Flash-Next IQ4_NL measured:

| PM4 | pp512 | tg128 |
|---|---:|---:|
| enabled | 630.95, 645.20 -> 638.08 | 31.33, 30.25 -> 30.79 |
| disabled | 625.71, 633.62 -> 629.67 | 29.49, 28.60 -> 29.05 |
| delta | +1.34% | +6.0% |

The PM4-disabled mean remains in the same range as the earlier 28.90 measurement, so the new path does not regress the fallback arm. The retained PM4 arm is now materially faster for token generation.

The scratch-recovery probe also passes after correcting its stale unpublished-packet setup:

```text
injecting error=0x401 with no pending dispatch at dispatch_id=1
queue_signal=-1 callback_status=4096 (HSA_STATUS_ERROR: A generic error has occurred.)
rocr_scratch_recovery=passed
```

The ROCr component gate, which includes the queue-profiling and runtime-binary checks, passed.

`git diff --check` is clean. The historical 3-dispatch/77-dword observations remain useful as revision history; the current cap is the 20-bit IB field rather than the old 1984-dword frame budget.

The runtime is installed directly into `C:\venv_torch\Lib\site-packages\_rocm_sdk_devel` and the split `_rocm_sdk_core` package by the component scripts; `build\runtime-backup` keeps the previous DLLs for rollback.

### Submitting the stream by reference

A command stream that does not fit a frame is now submitted through the dedicated variable-size buffer described above. The earlier experiment that emitted an indirect-buffer jump from the frame into a separate allocation still does not work: `D3DKMTSubmitCommandToHwQueue` receives exactly one command buffer address and length through `Wkmi::FillinSubmitPrivData`, and only the command buffer passed to the submit call is fetchable. The variable-size buffer makes the stream itself the submitted command buffer; direct submission of the graph IB without copying remains an unqualified experiment.

Submitting a stream through a variable-size command buffer is therefore part of the current tree. It is not the same as the rejected by-reference jump: the stream is copied once into a fetchable command buffer, and the completion trailer and ring-rptr update are appended there.

### Rebuild note

The ROCr and CLR build trees do not record header dependencies (`ninja -t deps` reports `#deps 0`). After changing `queue.h` or any other class layout used by `ComputeQueue`, delete the affected thunk/static-runtime object directories (or clean the target) before rebuilding. A stale `queues.cpp.obj` produced heap corruption during the first variable-buffer attempt.
