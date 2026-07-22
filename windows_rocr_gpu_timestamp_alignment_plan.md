# Windows ROCr GPU timestamp alignment plan

## Status

The runtime fix is implemented and validated on the `rocprofiler-windows` branch. The defect belongs to the Windows ROCr GPU timestamp translation path. It is not owned by the native `rocprofv3` port, although HIP events and ROCProfiler both consume the affected HSA profiling timestamps.

The validated environment is:

- native Windows;
- active Python environment `C:\venv_torch`;
- GPU `AMD Radeon(TM) 8060S Graphics`;
- architecture `gfx1151`;
- PyTorch `2.12.0+rocm7.15.0a20260721`;
- HIP `7.15.26286`;
- package-owned HIP runtime with ROCr embedded in `amdhip64_7.dll`.

## Ownership and scope

This work is scoped to:

- `projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp`;
- Windows conversion of AQL dispatch and async-copy timestamps into the system clock domain;
- HIP event elapsed-time behavior that consumes those converted timestamps;
- a focused installed-PyTorch regression probe in the existing CLR Windows runtime build script.

This work does not change:

- ROCProfiler SDK counter collection or trace policy;
- `rocprofv3` command behavior;
- Linux timestamp validation or conversion;
- the Windows build, install, or split-package architecture;
- the requirement that ROCr remain embedded in `amdhip64_7.dll`;
- `C:\Windows\System32\amdhip64_7.dll`.

## Upstream provenance

The defect predates the Windows ROCProfiler work.

The then-current `origin/develop` source and the branch merge base contained the same sequence:

1. Reject raw nonzero AQL timestamps when they are less than the GPU calibration captured at runtime startup.
2. Return zero timestamps without attempting Windows epoch correction.
3. Correct an epoch mismatch only when the translated timestamp is in the future.

The earlier upstream fix `rocr: Fix GPU timestamp alignment on Windows (#6282)` was already present in both `origin/develop` and `rocprofiler-windows`. It addressed a measured AQL epoch approximately 149 ms ahead of `D3DKMTQueryClockCalibration`. It did not cover an AQL epoch behind the calibration, and the earlier raw-timestamp validation prevented that case from reaching the correction logic.

The Windows ROCProfiler commits did not modify this `TranslateTime` block. The baseline WDDM queue already emitted `start_ts` and `end_ts` with `BuildCopyData` when profiling was enabled.

## Affected call paths

The HIP event path is:

```text
torch.cuda.Event.elapsed_time()
  -> c10::cuda::CUDAEvent::elapsed_time()
  -> hipEventElapsedTime()
  -> hip::Event::elapsedTime()
  -> hip::EventDD::time()
  -> roc::Device::getHwEventTime()
  -> hsa_amd_profiling_get_dispatch_time()
  -> AMD::GpuAgent::TranslateTime()
```

ROCProfiler dispatch tracing and other HSA profiling consumers enter the same ROCr translation path through `hsa_amd_profiling_get_dispatch_time()` or `hsa_amd_profiling_get_async_copy_time()`.

PyTorch performs event validation but does not transform the elapsed-time result. A standalone PyTorch process reproduced the failure without loading a ROCProfiler tool, which establishes that the port was not causal.

## Failure evidence

The failing machine reported values in different GPU clock epochs:

```text
AQL raw event timestamp:          approximately 1.418e12 ticks
D3DKMT startup calibration:       approximately 2.116e13 ticks
AQL epoch position:               approximately 197,411 seconds behind
```

The clocks advanced at the same effective frequency but had different bases, consistent with the AQL-visible GPU timestamp epoch having reset before process startup.

The original Windows path treated `raw < t0.GPUClockCounter` as invalid and returned zero. HIP then fell back to unsuitable command profiling timestamps. Observed `torch.cuda.Event.elapsed_time()` results were:

```text
operations=1       event_ms=0.0
operations=10      event_ms=0.0
operations=100     event_ms=0.0
operations=1000    event_ms=-211623888.0
```

The invalid estimate caused benchmark frameworks to derive excessive repetition counts or select kernels using negative timing values.

## Required behavior

The Windows implementation must satisfy these invariants:

1. Zero hardware timestamps remain invalid.
2. A nonzero Windows AQL timestamp below the startup calibration is allowed to reach epoch correction.
3. A timestamp that translates before runtime startup proves a negative epoch mismatch.
4. A timestamp that translates into the future proves a positive epoch mismatch.
5. The signed GPU tick offset is applied consistently to dispatch and async-copy timestamps.
6. Event intervals remain positive and track synchronized host duration within normal launch and synchronization overhead.
7. Linux retains its existing `raw >= t0` validation and translation path.
8. No separate `hsa-runtime64.dll` dependency is introduced into the installed HIP runtime.
9. The active `_rocm_sdk_devel` and `_rocm_sdk_core` runtime copies remain identical.
10. System32 is never modified.

## Implementation

### ROCr translation

The Windows dispatch and async-copy validation now rejects zero timestamps but does not reject a nonzero timestamp solely because it is less than `t0_.GPUClockCounter`.

The existing Windows epoch detection now accepts either proof of mismatch:

- translated timestamp greater than the current system timestamp;
- translated timestamp less than `t0_.SystemClockCounter`.

The correction computes a signed GPU tick offset, retranslates the first timestamp, and reuses the offset for subsequent timestamps. Linux remains behind the original validation condition and does not enter the Windows correction block.

### Regression probe

The existing installed-PyTorch HIPK probe in `projects/clr/scripts/build_windows_runtime.ps1` now:

1. Creates timing-enabled start and stop events.
2. Records 100 GPU operations between them.
3. Synchronizes the stop event.
4. Requires `0.0 < elapsed_ms < 60000.0`.

This is intentionally part of the existing runtime probe rather than a new build mode. It validates the installed package-owned runtime and catches zero, negative, non-finite-by-comparison, and implausibly large values.

## Completed validation

- [x] Reproduced zero and negative `torch.cuda.Event.elapsed_time()` results.
- [x] Captured raw AQL and D3DKMT calibration values from the failing runtime.
- [x] Confirmed PyTorch forwards directly to `hipEventElapsedTime()`.
- [x] Confirmed the same defective logic exists in `origin/develop` and the branch merge base.
- [x] Rebuilt the embedded CLR/ROCr runtime with the two-sided correction.
- [x] Installed the runtime through the normal Windows runtime script.
- [x] Passed the installed PyTorch event regression with `event_elapsed_ms=0.648377001285553`.
- [x] Passed delayed event query validation.
- [x] Passed CLR Windows tests, 3 of 3.
- [x] Passed standalone ROCr Windows tests, 3 of 3.
- [x] Passed AQL Profile Windows tests, 2 of 2.
- [x] Passed the complete ROCProfiler SDK Windows graph at the timestamp-fix gate, 37 of 37; the later dispatch-analysis graph passed 41 of 41 with the correction still installed.
- [x] Completed `benchmark_mm_hip_fp16.py --sizes 128` within 10 seconds.
- [x] Verified PowerShell syntax and `git diff --check`.
- [x] Verified no temporary timestamp diagnostics remain in `amdhip64_7.dll`.
- [x] Verified the active split runtime copies are byte-identical.
- [x] Verified the System32 HIP runtime remains unchanged.

Representative corrected timings were:

```text
operations=1       event_ms=145.780    host_ms=146.013
operations=10      event_ms=0.585      host_ms=0.767
operations=100     event_ms=2.048      host_ms=2.161
operations=1000    event_ms=16.719     host_ms=16.845
```

Generated runtime and System32 library hashes are intentionally not recorded because those artifacts change between builds. Validation requires byte-identical active split runtime copies and proves that the pre-existing System32 runtime is unchanged.

## Remaining work

- [ ] Submit the ROCr correction to upstream `develop`, referencing the limitation in #6282.
- [ ] Add a deterministic ROCr unit test with injectable calibration, raw GPU ticks, and system time so positive and negative epoch offsets do not require a particular machine state.
- [ ] Validate the unchanged Linux branch in a native Linux environment.
- [ ] Evaluate recalibration if the AQL GPU clock epoch changes after an offset has already been established in a long-running process.
- [ ] Evaluate whether absolute timestamps need a tighter first-sample alignment contract; interval timing is accurate because the common epoch offset cancels, while first-sample absolute alignment inherits the existing approximation to observation time.

## Acceptance criteria

The runtime fix is ready for branch use when:

- timing-enabled HIP events return finite positive intervals for real GPU work;
- event intervals scale with operation count and remain consistent with synchronized host timing;
- Windows dispatch and async-copy profiling accept AQL epochs on either side of the D3DKMT calibration;
- Linux behavior is unchanged by construction;
- CLR, ROCr, AQL Profile, and ROCProfiler Windows validation remain green;
- the installed split runtime preserves embedded ROCr, HIPK support, package-owned DLL resolution, and the protected System32 invariant.

All branch-use acceptance criteria are met in the validated Windows environment. Native Linux execution and upstream submission remain follow-up work.
