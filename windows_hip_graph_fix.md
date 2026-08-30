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
