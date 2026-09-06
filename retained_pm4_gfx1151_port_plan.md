# Retained PM4 Graph Execution on gfx1151

This document records the implemented design and validation of the retained-PM4 graph path for the local `gfx1151` machine. It is intentionally scoped to the behavior that has been ported and tested; it is not an installation guide or a step-by-step port plan.

## Status and Scope

The implementation combines four relevant parts of pwilkin's fork:
- `4f1d7ef589` (`hip: fix quadratic case, optimize memory usage`): graph parameter storage, graph-update caching, packet reuse, synchronization, recapture, and graph-exec setter updates.
- `2f725f1807` (`hip: merge collapsed graph packet batches`): optional merging of collapsed single-stream packet batches.
- `78d1160060` (`fix: prefer selected ROCr headers in HIP build`): selected-ROCr-header integration for the paired HIP build.
- The gfx1151-relevant portions of `04210923fe` (`hip: add retained PM4 graph command lists`): the ROCr vendor API, GFX11 encoder, ROCclr integration, HIP graph selection, tests, and documentation.

The port preserves the existing local PC-sampling, thread-tracing, and late-DSO work. It does not change `install.sh`, install packages, download models, or add llama.cpp behavior.

The retained-PM4 implementation is qualified only for:

```text
major == 11
minor == 5
stepping == 1
```

That is the `gfx1151` device used for validation. The GFX12-specific fork change `c4b77ac5cb` was not ported, and `f2d302f951` from `pwilkin/hip-update-fix` was excluded because it targets metadata-prefetch behavior that is not enabled on this machine.

## Design

### Graph Parameter and Update Path

The HIP graph execution path now uses the fork's contiguous, aligned parameter storage and explicit overflow/input validation. Graph executable updates retain enough topology and node information to distinguish unchanged graphs, parameter-only changes, and structural changes without repeatedly reconstructing all state.

The update path also includes:
- Topology-version tracking and cached update ordering.
- Reusable packet, metadata, and kernarg capture storage.
- Batched AQL packet updates.
- Safe kernarg-slot reuse.
- A shared execution/update lock.
- Recapture when direct packet patching is not valid.
- Disabled-node rebuild handling that preserves dependency waits and completion signals.
- Common packet-refresh handling for kernel, memcpy, memset, event, cooperative, child-graph, symbol, and batch-memory-operation setters.

`hipGraphExecBatchMemOpNodeSetParams` uses the common graph-executable update path, so it does not maintain a separate recapture mechanism.

The result is an optimized ordinary AQL graph path as well as the foundation needed to invalidate and rebuild retained PM4 state correctly after graph updates.

### Collapsed Graph Batch Merging

When the existing graph scheduler collapses a graph onto one stream, `DEBUG_HIP_GRAPH_MERGE_COLLAPSED=1` can merge its packet batches into one batch. Merging is deliberately restricted to graphs that are:
- Fully captured.
- Child-graph free.
- Represented by one packet batch per segment.

The merge preserves dependency ordering and inserts the required join behavior when independently rooted segments are concatenated. The flag defaults to `0`, so existing collapsed scheduling behavior remains the default.

### ROCr Graph Command-List API

ROCr exposes an optional vendor API in `hsa_ven_amd_graph.h` for:
- Querying graph-command capabilities.
- Creating and destroying opaque command lists.
- Querying materialized command-list information.
- Materializing a command list into a queue-specific vendor packet.

The API is exported by the HSA runtime shared library and is loaded dynamically by ROCclr. If the symbols are absent, the capability query fails, command-list creation fails, or packet materialization fails, HIP continues with its normal AQL submission path.

The API's command-list description carries dispatch packets, dependency kinds, and per-kernel flags. ROCr obtains the code-object kernel descriptor through the loader and uses it to construct the PM4 image without requiring HIP to know the hardware register layout.

### GFX11 PM4 Encoder

The retained encoder is implemented for GFX11 and uses code-object metadata to program the compute registers and dispatch packets. It emits the required PM4 setup, dependency, cache-acquire, dispatch, and compute-idle sequences, including a patchable temporary-ring register for scratch use.

Validation is conservative. The encoder rejects unsupported or unsafe cases, including:
- Dynamic call stacks.
- Unsupported kernel-code properties.
- Invalid code-entry alignment.
- Kernarg preload usage.
- Invalid workgroup/grid divisibility.
- Excessive LDS allocation.
- Unsupported private-segment and scratch combinations.
- Missing kernarg addresses when required.
- Unverified VMEM-only dependencies.

The encoder has golden-word, ABI-boundary, LDS, scratch-register, and capability-table tests. The capability table reports GFX11 as compile-supported but marks only `gfx1151` as runtime-qualified.

### ROCclr Materialization

ROCclr adds `GraphPm4Batch` as the backend-neutral retained batch abstraction and `RocrGraphPm4Batch` for the ROCr implementation. The ROCr backend:
- Checks the runtime capability and graph eligibility.
- Converts the flat HIP dispatch packets to the ROCr command-list description.
- Loads kernel metadata and rejects packets that cannot be encoded safely.
- Creates and retains the opaque ROCr command list.
- Queries dispatch count, PM4 dword count, and maximum private-segment size.
- Materializes a queue-specific vendor packet at launch time.
- Submits the packet through the native AQL queue.

Command-list ownership is reference-counted through the graph batch object. Scratch state is retained with the batch and associated with the queue so that queued work cannot observe resources being reclaimed prematurely.

### HIP Graph PM4 Selection

Retained PM4 is an explicit opt-in optimization. A graph batch must satisfy all of the following before ROCclr attempts materialization:
- `DEBUG_HIP_GRAPH_PM4=1`.
- The runtime reports the exact qualified `gfx1151` capability.
- The batch contains at least two dispatches.
- All packets are kernel dispatches.
- No per-dispatch completion signals are present.
- No disabled-node or unsupported graph state requires a fallback path.
- Dispatch profiling is not active.
- Kernel objects and code-object metadata are available.
- Encoder validation succeeds.

PM4 caches are invalidated when AQL batches are rebuilt or graph updates recapture packets. Graph execution and update operations share the relevant lock, and retained command lists and scratch leases remain alive until queued work has completed.

## Eligibility and Fallback

The design is fail-closed. A graph that is not provably safe for retained PM4 remains an AQL graph. This includes unsupported kernels, child graphs, non-kernel packets, completion-signal packets, profiling paths, unqualified ASICs, missing ROCr symbols, failed metadata lookup, and failed command-list materialization.

The unqualified override exists as a separate debug flag for development, but it is not used for this machine and is not part of the normal configuration:

```text
DEBUG_HIP_GRAPH_PM4=0
DEBUG_HIP_GRAPH_PM4_UNQUALIFIED=0
DEBUG_HIP_GRAPH_MERGE_COLLAPSED=0
```

PM4 should remain opt-in until a broader benchmark comparison establishes that it improves the target workload without reducing graph compatibility.

## Files and Integration Points

The main implementation areas are:
- `projects/clr/hipamd/src/hip_graph.cpp`
- `projects/clr/hipamd/src/hip_graph_internal.cpp`
- `projects/clr/hipamd/src/hip_graph_internal.hpp`
- `projects/clr/rocclr/platform/command.hpp`
- `projects/clr/rocclr/device/rocm/rocrctx.cpp`
- `projects/clr/rocclr/device/rocm/rocrctx.hpp`
- `projects/clr/rocclr/device/rocm/rocvirtual.cpp`
- `projects/clr/rocclr/device/rocm/rocvirtual.hpp`
- `projects/clr/rocclr/utils/flags.hpp`
- `projects/clr/rocclr/cmake/ROCclrHSA.cmake`
- `projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ven_amd_graph.h`
- `projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_graph_command_encoder.h`
- `projects/rocr-runtime/runtime/hsa-runtime/core/runtime/hsa_ven_amd_graph.cpp`
- ROCr CMake and shared-library export definitions.

The focused tests and documentation are:
- `projects/rocr-runtime/rocrtst/suites/functional/graph/graph_command_encoder_test.cc`
- `projects/hip-tests/catch/unit/graph/hipGraphExecUpdate.cc`
- `projects/hip-tests/catch/unit/graph/hipGraphExecBatchMemOpNodeSetParams.cc`
- `projects/hip-tests/catch/performance/scenarios/graph/hipGraphCollapseCorrectness.cc`
- `projects/rocr-runtime/runtime/docs/contribution/retained-pm4-command-lists.rst`
- `projects/rocr-runtime/runtime/docs/index.rst`
- `projects/rocr-runtime/runtime/docs/sphinx/_toc.yml.in`

The retained-PM4 documentation records the experimental status, gfx1151 qualification, AQL fallback, scratch and lifetime constraints, fork-author benchmark provenance, and adapted-code licensing information. GFX12-specific retained-PM4 claims are not presented as applicable to gfx1151.

## Build and Install Integration

The existing private-prefix workflow builds and installs the paired libraries without changing the installer:

```sh
./build_rocr.sh
./build_hip_runtime.sh
```

Both scripts use `ROCM_PATH` as the install prefix and invoke `sync_rocm_sdk_links.py` to update the development-library aliases while preserving backups of previous version links. The resulting ROCr and HIP libraries, headers, CMake metadata, and graph API exports are available from the existing Python virtual-environment SDK prefix.

For GPU validation on this machine, `HSA_DISABLE_XDNA=1` is used to bypass the unrelated local XDNA discovery problem. This environment setting is not part of the retained-PM4 design.

## Validation

The focused build and test validation completed against the installed ROCr/HIP pair:
- `./build_rocr.sh`: passed, including ROCr, `rocrtst64`, and KFD test builds.
- `./build_hip_runtime.sh`: passed, including `libamdhip64` and HIPRTC installation.
- ROCr `graph_command_encoder_test`: 1 test passed.
- HIP `Unit_hipGraphExecUpdate*` plus `Unit_hipGraphExecBatchMemOpNodeSetParams*`: 27 tests passed, 1 peer-access test skipped because peer access is unavailable, and 235 assertions passed.
- `Performance_Graph_CollapseCorrectness` and `Performance_Graph_CollapsedIndependentCompletion`: 220 assertions passed with merging disabled and 220 assertions passed with merging enabled.
- `Performance_Graph_RmwAndAsyncDestroy`, `Performance_Graph_RmwUpdateAndDisable`, `Performance_Graph_Pm4ConcurrentLaunchAndTrim`, `Performance_Graph_Pm4QueueScratch`, and `Performance_Graph_Pm4QueueScratchConcurrentLaunchAndTrim`: 5 tests and 134 assertions passed with retained PM4 enabled.

The retained path was confirmed active rather than merely falling back to AQL. With `DEBUG_HIP_GRAPH_PM4=1` and `AMD_LOG_LEVEL=3`, the runtime reported:

```text
[hipGraph][PM4] retained 8 dispatches in 167 dwords, max private 48 bytes
```

The validation was intentionally focused on the ported graph and ROCr behavior. It did not run the entire repository test suite or the llama.cpp test suite. A targeted llama.cpp benchmark was run separately against the already-built fork-patched binaries to measure the retained-PM4 effect on the target machine.

### Qwen3.6-35B-A3B Workload Benchmark

The benchmark used the local `gfx1151` Radeon 8060S and:
- Model: `Qwen3.6-35B-A3B-APEX-I-Quality.gguf`, Q6_K, 34.66B parameters, 22.8 GB.
- Binary: `~/llama.cpp/build/bin/llama-bench`.
- Runtime: `-ngl 99 -fa on -b 2048 -ub 512 -dev ROCm0` with five repetitions per workload.
- Common environment: `HSA_DISABLE_XDNA=1`, `ROCBLAS_USE_HIPBLASLT=1`, `DEBUG_HIP_GRAPH_MIN_OVERLAP=0`, and `DEBUG_HIP_GRAPH_MERGE_COLLAPSED=0`.
- Comparison: `DEBUG_HIP_GRAPH_PM4=0` versus `DEBUG_HIP_GRAPH_PM4=1`.

`pp` denotes prompt processing and `tg` denotes token generation. Throughput is tokens per second; the uncertainty is the benchmark's reported standard deviation over five samples.

| Workload | PM4 disabled | PM4 enabled | Change |
| --- | ---: | ---: | ---: |
| `pp512` | 1327.20 +/- 13.90 | 1346.86 +/- 18.92 | +1.48% |
| `pp2048` | 1309.24 +/- 5.04 | 1305.63 +/- 2.22 | -0.28% |
| `tg128` | 58.60 +/- 0.08 | 68.86 +/- 0.12 | +17.51% |
| `tg512` | 58.67 +/- 0.02 | 69.06 +/- 0.03 | +17.71% |

The timed runs did not enable verbose logging. A separate diagnostic run with `DEBUG_HIP_GRAPH_PM4=1` and `AMD_LOG_LEVEL=3` confirmed that the optimized path was used on this workload:

```text
[hipGraph][PM4] retained 2685 dispatches in 80948 dwords, max private 0 bytes
```

The equivalent diagnostic run with `DEBUG_HIP_GRAPH_PM4=0` emitted no retained-PM4 message. On this machine and model, retained PM4 is therefore strongly beneficial for generation and effectively neutral for prompt processing. The result supports recommending `DEBUG_HIP_GRAPH_PM4=1` for this workload while keeping the feature opt-in until more models and graph shapes are measured. No broad llama.cpp test suite was run; this was a targeted benchmark only.

### DeepSeek-V4 Workload Benchmark

A second benchmark used the local `gfx1151` Radeon 8060S and:
- Model: `DeepSeek-V4-Flash-IQ2XXS-0731.gguf`, IQ2_XXS, 284.3B parameters, 81.0 GiB.
- Binary: `~/llama.cpp/build/bin/llama-bench`.
- Runtime: `-ngl 99 -fa on -b 2048 -ub 512 -dev ROCm0` with five repetitions per workload.
- Common environment: `HSA_DISABLE_XDNA=1`, `ROCBLAS_USE_HIPBLASLT=1`, `DEBUG_HIP_GRAPH_MIN_OVERLAP=0`, and `DEBUG_HIP_GRAPH_MERGE_COLLAPSED=0`.
- Comparison: `DEBUG_HIP_GRAPH_PM4=0` versus `DEBUG_HIP_GRAPH_PM4=1`.

| Workload | PM4 disabled | PM4 enabled | Change |
| --- | ---: | ---: | ---: |
| `pp512` | 201.03 +/- 1.25 | 203.10 +/- 1.84 | +1.03% |
| `pp2048` | 195.16 +/- 0.79 | 195.41 +/- 0.94 | +0.13% |
| `tg128` | 14.81 +/- 0.02 | 17.18 +/- 0.17 | +16.00% |
| `tg512` | 14.73 +/- 0.05 | 17.13 +/- 0.04 | +16.28% |

A generation diagnostic with PM4 enabled and `AMD_LOG_LEVEL=3` confirmed retained execution for this larger graph:

```text
[hipGraph][PM4] retained 4675 dispatches in 134045 dwords, max private 0 bytes
```

The corresponding generation run completed with successful HIP graph launches. As with Qwen3.6, PM4 provides a large generation improvement while prompt processing is effectively unchanged. The two-model result strengthens the case for enabling `DEBUG_HIP_GRAPH_PM4=1` in the target llama.cpp environment, while retaining the runtime's opt-in default.

### DeepSeek V4 Loading Mode

The DeepSeek benchmark used `--load-mode dio` because that was the previously established conservative setting for this unified-memory machine. A guarded probe using the default `--load-mode auto` also loaded the full model successfully without an OOM or duplicate model allocation. The loader reported:

```text
load_tensors: loading model tensors, this can take a while... (load_mode = none)
ROCm0 model buffer size = 81687.67 MiB
ROCm_Host model buffer size = 1010.00 MiB
```

For the current HIP build, `auto` disables mmap for the integrated `gfx1151` device and resolves to ordinary loading (`none`). Therefore `--load-mode dio` is no longer required for correctness or to avoid the mmap/file-cache duplication problem. It remains an optional way to bypass filesystem page-cache effects. Explicit `--load-mode mmap` should not be used on this machine.

This behavior is consistent with the Strix Halo unified-memory concern discussed in [safetensors PR #728](https://github.com/huggingface/safetensors/pull/728), but the llama.cpp conclusion is based on its local loader/device-capability path and the successful guarded load test. No broad llama.cpp test suite was run; these were targeted benchmarks and loading validation only.

## Constraints and Follow-Up

The current design intentionally leaves these constraints in place:
- GFX12 retained PM4 requires a separate architecture-specific design and qualification effort.
- PM4 remains disabled by default.
- AQL is the compatibility and failure fallback.
- Performance claims must be established with target workload measurements; the fork's reported numbers are provenance, not guarantees for this machine.
- Installer behavior, model downloads, and unrelated runtime changes remain outside this design.
