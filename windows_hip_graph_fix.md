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

The worktree retains only these source changes:

### CLR metadata publication

`projects/clr/rocclr/device/rocm/rocvirtual.cpp`:
- On the non-`MOVDIR64B` path, write each metadata segment with an invalid header followed by its body.
- Fence the non-temporal body writes.
- Release-publish the captured valid header for each segment only after the fence.
- Keep the `MOVDIR64B` path as a complete 64-byte body-plus-header write.

This restores body-first/header-last publication for metadata without changing the atomic full-segment path.

### ROCr scratch recovery

`projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_aql_queue.h` and `projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp`:
- Change `HandleInsufficientScratch` from `void` to `bool` so failure to find a usable dispatch packet is propagated.
- Acquire-load packet headers while scanning for the dispatch that needs scratch.
- Yield and retry unpublished headers up to 64 times, then return failure instead of dereferencing a null packet.
- Convert dynamic queue exhaustion into a fatal generic HSA error.
- Treat a WDDM `error_code == -1` as `HSA_STATUS_ERROR`. It is a generic queue-failure sentinel, not the `0x401` insufficient-scratch bitmask.

The diagnostic-only WDDM logging edits in `projects/rocr-runtime/libhsakmt/src/dxg/wddm/queue.cpp` were reverted and are not part of this change.

## Build and install

The ROCr runtime was rebuilt and installed with:

```powershell
C:\rocm-systems\projects\rocr-runtime\scripts\build_windows.ps1 `
  -VenvPath C:\venv_torch `
  -BuildDirectory C:\rocm-systems\build\rocr-runtime `
  -InstallPrefix C:\venv_torch\Lib\site-packages\_rocm_sdk_devel `
  -BuildType RelWithDebInfo
```

The HIP DLL embeds a static ROCr runtime, so rebuild the configured CLR target after changing either ROCr or CLR sources:

```powershell
cmake --build C:\rocm-systems\build\clr-hip `
  --config RelWithDebInfo `
  --target amdhip64
```

The fixed DLL used below was produced at:

```text
C:\rocm-systems\build\clr-hip\hipamd\src\amdhip64_7.dll
```

## Component validation

The four Windows ROCr component tests were run with:

```powershell
ctest --test-dir C:\rocm-systems\build\rocr-runtime `
  -C RelWithDebInfo `
  -R "hsakmt\.windows\.(packet-publication|profiling-adapter)|rocr\.windows\.(queue-profiling|runtime-binary)" `
  --output-on-failure
```

Observed result:

```text
1/4 Test #1: hsakmt.windows.packet-publication ...   Passed
2/4 Test #2: hsakmt.windows.profiling-adapter ....   Passed
3/4 Test #3: rocr.windows.queue-profiling ........   Passed
4/4 Test #4: rocr.windows.runtime-binary .........   Passed

100% tests passed out of 4
```

Also run:

```powershell
git -C C:\rocm-systems diff --check
```

## Fixed-runtime regression test

The probe is stored in `rocr_scratch_recovery_test.cpp`. The build and run wrapper is `build_rocr_scratch_recovery_test.ps1`.

The probe enters the ROCr dynamic queue event handler used for scratch recovery. It creates a GPU HSA queue, leaves dispatch slot zero invalid, sets the internal dispatch range to `[0, 1)`, and injects the `0x401` insufficient-scratch event through the queue inactive signal.

The fixed runtime observes the invalid header, retries with yielding, treats the event as fatal, suspends the queue, reports `HSA_STATUS_ERROR`, and completes the event with signal value `-1`.

### Build and run with the fix

Rebuild the ROCr runtime and the embedded HIP DLL using the commands above. Then run the wrapper from the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File C:\rocm-systems\build_rocr_scratch_recovery_test.ps1 -Run
```

The script compiles the probe against `amdhip64.lib`, stages the fixed DLL from `C:\rocm-systems\build\clr-hip\hipamd\src\amdhip64_7.dll` beside the executable, adds the package `bin` and `lib` directories to `PATH`, and runs the staged executable. The executable directory controls DLL selection, and the probe prints the loaded DLL path.

To use a different fixed build, pass its DLL explicitly:

```powershell
powershell -ExecutionPolicy Bypass -File C:\rocm-systems\build_rocr_scratch_recovery_test.ps1 `
  -RuntimeDll C:\path\to\amdhip64_7.dll `
  -Run
```

Expected fixed-runtime output includes:

```text
amdhip64=C:\rocm-systems\build\rocr-scratch-recovery\fixed\amdhip64_7.dll
injecting error=0x401 with unpublished packet at dispatch_id=0
queue_signal=-1 callback_status=4096 (HSA_STATUS_ERROR: A generic error has occurred.)
rocr_scratch_recovery=passed
```

The wrapper returns exit code `0` only when the queue signal reaches `-1`, the callback reports `HSA_STATUS_ERROR`, and the probe prints `rocr_scratch_recovery=passed`.

## Local metadata limitation

The local HSA queue query returned metadata prefetch version `255.255` and metadata ring address `0x0`. Metadata prefetch is therefore not enabled on the queue path available on this machine. The direct graph metadata race cannot be reproduced here. The fixed-runtime regression above is intentionally limited to the ROCr unpublished-packet recovery boundary.

For separate graph-path validation on a machine with metadata prefetch support, force the relevant non-`MOVDIR64B` and system-memory paths:

```powershell
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

`validation status 3` is `ValidationStatus::kInvalidManifest`. HIP lowered the graph to a retained PM4 command list (`hsa_ven_amd_graph_*`), and ROCR materialized it as an AQL vendor packet with `vendor_header = 1`, `dword_count_remaining = 10`, and a canonical `IT_INDIRECT_BUFFER` header, but left `reserved[0..7]` zero. The WDDM thunk accepts a format-1 packet only with a manifest (`WCMP` for profile packets, `WRTM` for runtime packets), so it rejected the packet and failed the queue. Even with a manifest, the thunk accepted only IB control bits of `1 << 23`, while the graph encoder also sets the temporal last-use bits (`3 << 28`). A second, independent limit applies: the thunk copies the whole indirect buffer into one fixed-size PM4 frame, so a 139627-dword list (about 558 KB) can never be submitted through a single AQL slot; the validated budget is 1984 dwords (`(kQualifiedFrameBytes - kFrameTrailerReserveBytes) / 4 = (0x2000 - 0x100) / 4`), so the graph is about 70 times over budget.

Linux has no such validator, which is why the same commit works there.

### Retained fix

- `projects/rocr-runtime/runtime/hsa-runtime/core/runtime/hsa_ven_amd_graph.cpp`
  - `Materialize` stamps the runtime manifest (`WRTM`, version, command dword count, FNV checksum over the indirect buffer) on Windows, matching `AqlQueue::ExecutePM4`.
  - `Create` rejects a command list larger than the thunk's `MaxPm4Dwords` with `HSA_STATUS_ERROR_OUT_OF_RESOURCES`, so HIP falls back to the AQL batch path for that graph instead of submitting a packet the thunk must reject.
- `projects/rocr-runtime/libhsakmt/include/impl/wddm/profiling.h`
  - `ValidateRuntimePacket` accepts the optional temporal last-use IB control bits in addition to `IB_VALID`; every other control bit, the size, the checksum, the memory ownership, and the completion signal are still checked.
- `projects/rocr-runtime/runtime/docs/contribution/retained-pm4-command-lists.rst` records the Windows packet budget and manifest requirement.

### Verification

Small graph (3 dispatches, 77 dwords), `build\small_graph_test.cpp`, captured and replayed through HIP:

```text
unfixed runtime:  reject AQL Profile vendor packet: validation status 3 ... ib_header=c0023f00
                  queue fail status = 00001009, exit 8
fixed runtime:    [hipGraph][PM4] retained 3 dispatches in 77 dwords, max private 0 bytes
                  small_graph=passed dispatches=3 kernels=9 value=9.0
```

Large graph (600 dispatches, about 15k dwords):

```text
[hipGraph][PM4] ROCR command-list fallback for 600 packets: status=4104
small_graph=passed dispatches=600 kernels=1800 value=1800.0
```

End to end with the model command from the report (`-c 262144`):

```text
[hipGraph][PM4] ROCR command-list fallback for 4826 packets: status=4104
listening on http://127.0.0.1:8080
completion: " Paris, which is"
```

No thunk rejection, no GPU hang, and no queue abort appeared in the fixed run. The four ROCr component tests still pass.

The fixed `amdhip64_7.dll` was installed over `C:\venv_torch\Lib\site-packages\_rocm_sdk_devel\bin\amdhip64_7.dll`; the previous file is kept at `C:\rocm-systems\build\runtime-backup\amdhip64_7.dll` for rollback.

### Attempted: referencing the stream instead of inlining it

A command stream that does not fit a frame was submitted as the packet's own indirect buffer instead of being copied (`VendorSpecificAqlToPm4` emitted a 4-dword `IT_INDIRECT_BUFFER` jump and the trailing packets that order completion behind the stream). Two encodings were tried: the packet's jump words, and a canonical jump rebuilt from the validated address and dword count. The validator accepted both, the submission returned no error, and then the queue stalled: `hipGraphLaunch` reported success and `hipStreamSynchronize` never returned, while the same packet inlined into a frame completed normally.

The submission contract explains why. `D3DKMTSubmitCommandToHwQueue` receives exactly one command buffer address and length through `Wkmi::FillinSubmitPrivData`, which carries no allocation or residency list, and the thunk never emits an indirect-buffer jump for any vendor stream: profile packets are inlined too, even though Linux hands the same packets to the hardware as indirect buffers. Only the command buffer passed to the submit call is therefore fetchable, and a jump into a separate allocation stalls the command processor.

Enabling these graphs on Windows needs a thunk or KMD change: either declare the referenced allocation for the submission, or submit a variable-size command buffer that holds the whole stream plus the trailer.

Measured in the current fixed state, `DEBUG_HIP_GRAPH_PM4=1` and `=0` give pp512 122.57 +- 3.74 / tg128 15.22 +- 0.06 and pp512 122.23 +- 4.31 / tg128 14.96 +- 0.10 t/s for this model, because the graph is declined and both arms run the AQL batch path.
