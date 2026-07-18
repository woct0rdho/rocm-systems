# Thread Tracing on gfx1151 (Strix Halo)

## Goal

- Enable reliable Advanced Thread Trace (ATT) on gfx1151 without replacing upstream's generalized arbitrary multi-buffer SQTT architecture.
- Support detailed dispatch ATT, public triple-buffer mode, `NO_DETAIL`, bounded `--att-no-intercept`, selected regions, and bounded consecutive-kernel capture.
- Keep gfx115x register handling generation-specific while preserving generic gfx9/gfx12 behavior.
- Drain active device traces before HSA, queue, async-copy, and AQLprofile resources are destroyed.
- Establish queue-local `COMPUTE_THREAD_TRACE_ENABLE` state in the correct application-queue context while retaining internal control queues for generalized multi-buffer operation.

## Scope

- One gfx1151 GPU and one profiling process.
- Validation primarily uses target CU1 because CU0 can be scheduler/harvesting-dependent on this machine.
- Public `--att-triple-buffer` maps to `ROCPROFILER_THREAD_TRACE_PARAMETER_NUM_BUFFERS = 3`.
- The internal API remains arbitrary-buffer-count capable through `params.num_buffers`, per-slot CPU buffers, indexed chunks, and `read_offset`.
- There is no arbitrary public `--att-num-buffers` option.
- `HSA_DISABLE_XDNA=1` is used for GPU-only validation because the unrelated local XDNA path fails device open with `amdxdna_drm_open: SVA bind device failed, ret -19`.
- Ninja builds use their default all-core scheduling; GPU-resource tests run sequentially with `ctest -j1`.

## Final Status

### Passing

- Detailed gfx1151 triple-buffer capture produces decoded statistics and nonempty ATT output.
- Triple-buffer plus `NO_DETAIL` produces occupancy-oriented output and a nonempty ATT stream.
- The standalone `--att-no-detail` execution and validator pass with a fresh nonempty ATT payload.
- All enabled producer/consumer tests pass on gfx1151, including optional framing, reduced GPU capacity, `read_offset`, slow-consumer, and multi-slot ordering cases.
- Upstream's `stop_during_cpu_backpressure` and `gpu_full_disables_queue` producer/consumer cases pass on gfx1151.
- gfx11 quick scan recognizes TT header version 3 and reports dispatch/event boundaries.
- gfx11 standalone reconstruction can cut a quick-scanned range into a self-contained ATT buffer.
- Official selected-regions marker execution and validation now pass 2/2 with real decoded in-region data.
- The bounded `--att-no-intercept` fixture passes 3/3 once the in-tree decoder is installed: it writes a standalone `.att` file and a populated `ui_output_agent_*` directory (`build_trace_decoder.sh`).
- Immediate bounded consecutive-kernel capture produces a nonempty ATT stream; the latest run wrote a `5579360`-byte `.att` file plus UI output.
- `ROCPROF_ATT_LIBRARY_PATH` supports one path or an `os.pathsep`-separated path list; focused launcher tests pass.
- The no-intercept CTest fixture removes its output directory before execution so validation cannot pass against stale `ui_output_agent_*` data.
- `--selected-regions` plus `--att-consecutive-kernels` is rejected as a mutually exclusive combination.
- Generic resource teardown drains active device traces before destroying tracer agents.

### Known limitations

- **The no-intercept path needs the in-tree trace decoder.** `rocprofv3` links `librocprof-trace-decoder.so` at build time and resolves it from the SDK library directory at run time, so a decoder shipped by another ROCm SDK package in the same environment is what actually runs. The ROCm-wheel decoder in this venv reports zero quick-scan records for the gfx11 buffers this fork produces (verified by running both libraries on one captured 6.9 MB buffer through `rocprof_trace_decoder_quick_scan`: 0 records versus 29 dispatches), which leaves `--att-no-intercept` with no dispatch cuts and therefore no `.att` file and no `ui_output_agent_*` directory. Run `./build_trace_decoder.sh` after `build_rocprofiler.sh`; it installs the in-tree decoder and re-links any sibling ROCm SDK package copy that would shadow it.
- **The gfx11 SQTT engine can outrun the KFD re-arm round trip.** With a 1 MiB request the per-CU window is 790,528 bytes and fills in roughly 250 us, while a status query, swap packet and staging copy take longer than that; the engine then exhausts its internal write buffer, sets `BUF1_FULL | WRITE_BUF_FULL | PACKET_LOST_BUF_NO_LOCKDOWN | BUF_ISSUE` in STATUS2 and clears `CTRL.MODE` itself. This is recovered rather than terminal on gfx11 now (see below), so the capture keeps a trace with a gap instead of being discarded, but the underlying latency race is unresolved: larger `--att-buffer-size` values, or the HSA transport, do not hit it, while 1 MiB does on most runs.
- **`thread-trace-api-agent-before-hsa-init-test` fails.** The single-buffer device start requested before `hsa_init()` is replayed at HSA registration and then injected into the application queue on its first dispatch; SQTT programming issued that early is dropped by the hardware and the engine reads back `STATUS=0`/`STATUS2=0`/`WPTR=0` at stop. Delaying or repeating the same injection makes the identical packet effective: skipping the first four dispatches (`NI_SKIP_FIRST_START=4` in a scratch build) or re-injecting for the first eight dispatches produced 3.1-3.6 MB of trace data and passed the test. The sibling tests (`thread-trace-api-agent-test`, `-start-stop-before-hsa-init-test`, `-hsa-fallback-test`) pass; this defect predates the rebase and is not caused by it.
- Combining marker-controlled `--selected-regions` with `--att-triple-buffer` still produced a zero-byte final partial buffer in one focused probe. The supported selected-regions path is currently the single-buffer path; ordinary triple-buffer consecutive capture remains nonempty and validated.
- Multi-buffer application-queue activation currently covers queues that already exist when the device context starts. The active queue callback intentionally handles single-buffer queue-local control; a queue created after multi-buffer SQTT start is not yet activated by a dedicated multi-buffer queue-creation hook.
- The queue-local `COMPUTE_THREAD_TRACE_ENABLE` enable packet is built on demand and its `before_krn_pkt` list is empty until `populate_before()` runs; queue interposition does not call `populate_before()` for client packets, so the enable injection is currently a no-op for dispatches after the first one. The first dispatch's start packet still programs the queue-local state.
- The consecutive-kernel callback still uses function-static state and should be replaced with a per-device-context controller.
- Device stop waits and shader callbacks still run while `DeviceThreadTracer::agent_mut` is held; long GPU waits should move outside that mutex.
- The gfx11 quick scanner is SIMD-only and requires AVX-512 at runtime; there is no scalar fallback.

## Preserved Upstream Architecture

The rebased implementation keeps upstream's generalized multi-buffer design:
- `thread_trace_parameter_pack::num_buffers` controls the number of buffers.
- `att_queue_t::cpu_buffers` owns one CPU staging allocation per slot.
- Producer and consumer workers use per-slot ownership instead of a fixed legacy triple-buffer structure.
- Every callback carries `chunk_index` and `read_offset`.
- Tool output reassembles out-of-order callbacks by `chunk_index` and writes a contiguous ATT stream.
- Quick scan and standalone reconstruction operate on indexed chunks instead of assuming one monolithic buffer.

Public triple-buffer mode is therefore a request for three buffers through the generalized API, not restoration of the removed legacy buffering-mode API.

## gfx115x SQTT Details

- gfx1151 follows the gfx11 write-pointer model but needs gfx115x-specific `SQ_THREAD_TRACE_STATUS2`, `BUF1`, and `SQ_THREAD_TRACE_CTRL.DOUBLE_BUFFER` handling.
- Disabled shader engines retain 4 KiB address reservations. With a requested 1 MiB pool and 63 disabled SE slots, the active-SE capacity is `790528` bytes. In a two-enabled-SE run, each active SE reported `397312` bytes.
- AQLprofile's reported `status->size` is authoritative when it is no larger than the requested pool. The producer forwards that size instead of requiring equality with the requested allocation.
- Hardware `SQ_THREAD_TRACE_WPTR` values that fit within capacity are treated as offsets. Address-relative correction remains a fallback for preinitialized/mock values.
- `SQTTBufferingPackets` must be constructed while SQTT is idle, before SQTT start packets are submitted.
- The PM4 flush target and producer-visible `status->data` must identify the same completed previous buffer.
- Harvested-WGP SQ counter iteration and output sizing use the bitmap-derived active-WGP table rather than dense CU assumptions.
- Device mode programs `SQ_THREAD_TRACE_CTRL.ALL_VMID`; dispatch mode remains queue-scoped.
- Final AQLprofile snapshots are available at trace log level and include status, status2, counter, WPTR, base, capacity, and buffering mode.

## Application-Queue Ordering Fix

### Root cause

`COMPUTE_THREAD_TRACE_ENABLE` is queue-context state. Programming global SQTT state only on an internal control queue does not establish the required state on an application queue.

Early experiments added a first-application-queue start packet, but the packet was returned before its command buffer had been populated. Queue interposition copies `before_krn_pkt` directly; it does not call `populate_before()` for the client. The diagnostic log therefore showed that the branch executed while no start PM4 was actually injected. Immediate marker and consecutive captures ended with:
- `status = 0`
- `status2 = 0`
- `WPTR = 0`
- zero bytes of final shader data

Explicit gfx11 thread-trace event writes, larger buffers, alternate CUs/SIMDs, broader shader masks, and broad PMC preactivation did not correct that empty packet.

### Single-buffer gfx10/gfx11 device mode

For single-buffer gfx10/gfx11 device traces:
- `start_context()` reserves active-trace ownership but defers hardware start.
- The first application dispatch for the agent receives a cloned full SQTT start packet.
- The device callback explicitly calls `populate_before()` before returning the packet.
- The packet is serialized with the first target dispatch.
- `GpuSqttBuilder::Begin()` programs both queue-local `COMPUTE_THREAD_TRACE_ENABLE` and the global SQTT state on that application queue.
- Context stop submits the full single-buffer stop/readback packet on the most recently observed live application queue, waits for its completion signal, and only then iterates final data.

Application queues are tracked by HSA queue ID rather than by a potentially dangling raw pointer. `QueueController::get_queue(uint64_t)` resolves the currently live queue before stop submission.

This ordering changed the immediate selected-regions and consecutive snapshots from zero to nonzero WPTR. Focused 1 MiB/two-SE runs reported WPTR `0x307f` and wrote `397280` bytes.

### Multi-buffer gfx10/gfx11 device mode

Generalized multi-buffer mode retains the internal SQTT control queue and producer/consumer workers. Before internal SQTT start:
- `DeviceThreadTracer` iterates currently live application queues for the target agent.
- It directly submits one queue-local `COMPUTE_THREAD_TRACE_ENABLE` PM4 packet to each queue.
- It waits for every activation completion signal.
- Only then does it submit global SQTT start packets on the internal control queue.

This once-before-start ordering preserves `--att-no-intercept`: it does not require per-dispatch kernel interception for its six-buffer quick-scan path. Reinjecting queue-enable PM4 before every no-intercept dispatch was rejected after it caused a timeout and a recoverable amdgpu reset during queue removal.

### Queue callback safety

- The device queue callback is registered idempotently and removed during resource teardown.
- `.signal_completion = [](auto&&...) {}` is mandatory because the queue completion path invokes the callback unconditionally. Leaving it empty caused `std::bad_function_call` on the first application dispatch.
- New starts are rejected after process-wide shutdown begins.
- Queue activation is enabled only while a device context is active.
- The internal AQLprofile queue-control entry point is guarded at the SDK call site by `ROCPROFILER_EXTERNAL_AQLPROFILE == 0`.

## Producer/Consumer and Output Fixes

- Zero-length indexed END callbacks are preserved as boundaries.
- Zero-byte copies are skipped because they may never signal completion and can deadlock shutdown.
- The producer adopts upstream's KFD memory/signal transport: it builds its own `signal_ptr_t` submit signal, uses `att_queue_submit()`/`signal_wait()`, and no longer relies on a separate start-packet handshake.
- gfx11 startup polling yields aggressively for the first 5 ms and permits one zero-data restart after the 100 ms retry deadline; the restart is submitted through the same `restart_trace` callback that upstream uses after CPU backpressure.
- GPU status is rejected only when `status->size > buffer_size`; reduced architecture-adjusted capacity is valid.
- A true GPU overflow (`is_too_late`) is terminal: upstream's producer disables the queue, discards GPU-resident data, and rejects further packets instead of stopping and restarting the trace.
- Optional decoder framing is detected by callback size. Tests no longer assume chunk 0 is always a 32-byte warmup header; gfx115x may begin with real GPU data at chunk 0.
- ATT callbacks are reassembled by `chunk_index` before writing.
- The first chunk creates/truncates the `.att` file; subsequent chunks append.
- Repeated filenames are deduplicated to avoid redundant decode attempts.

## `--att-no-intercept`

The bounded path separates four earlier failure classes:
- Decoder discovery: `rocprofv3.py` splits `ROCPROF_ATT_LIBRARY_PATH` with `os.pathsep` and removes empty entries.
- Capture lifecycle: agents share `att_device_context`; the consecutive-dispatch callback starts it at the first targeted dispatch and calls `mark_started()`.
- gfx11 scanning: TT header version 3 routes to `gfx11::TokenLookupTable` through `scan_gfx11()`.
- Standalone reconstruction: gfxip 11 is accepted by `rocprof_trace_decoder_build_standalone()` and uses RDNA status-token reconstruction.

Finalization is deliberately ordered:
- `finalize_rocprofv3()` invokes `att_no_intercept::finalize()` before the SDK client finalizer destroys rocprofiler resources.
- Shared context handles are deduplicated and stopped once.
- Context stop drains producer/consumer work before decoder backends and `ThreadTracerAgent` objects are destroyed.
- Cleanup is idempotent because the tool finalizer can reach the same path later.

After the queue-order fix, the clean no-intercept fixture passed 3/3. The latest run produced a `1579457`-byte ATT file plus fresh UI output.

Rebase status: the fast path works once the in-tree decoder is installed (`./build_trace_decoder.sh`), which is what restores the `.att` file and `ui_output_agent_*` output; with the ROCm-wheel decoder that shadowed it, the quick scan returned no records at all. Three further fixes came out of investigating the flaky overflow: the tool's wait for the next chunk is bounded (5 s), the producer recovers from a gfx11 SQTT overflow by staging, stopping, draining and re-arming the trace instead of disabling the queue, and `ROCPROFILER_SQTT_DEBUG` makes the swap-time register state readable. The failure mode is now a recovered gap rather than a lost capture.

## Generic Device-Mode Finalization

The process-teardown ordering defect is addressed:
- `registration::finalize()` calls `thread_trace::finalize()` before async-copy and queue-controller teardown; upstream removed the separate `flush_and_stop()` helper and now relies on the KFD transport surviving `hsa_shut_down`.
- A process-global shutdown gate prevents a device context from submitting a new SQTT start after draining begins.
- `DeviceThreadTracer::resource_deinit()` explicitly stops, waits, and iterates final data before clearing `ThreadTracerAgent` objects.
- `ThreadTracerAgent` destruction remains a last-resort path, but waits for a single-buffer stop signal and performs final iteration instead of silently dropping the pending payload.
- `unit.thread_trace.resource_deinit_drains_active_device_trace` verifies active ownership is drained and the final shader callback executes before agent destruction.

Remaining lifecycle work is race/idempotence coverage, generation ownership, and moving waits/callbacks outside `agent_mut`.

## Relevant Files

- `projects/rocprofiler-sdk/source/bin/rocprofv3.py`
- `projects/rocprofiler-sdk/source/lib/rocprofiler-sdk-tool/tool.cpp`
- `projects/rocprofiler-sdk/source/lib/rocprofiler-sdk-tool/att_no_intercept.{hpp,cpp}`
- `projects/rocprofiler-sdk/source/lib/rocprofiler-sdk-tool/att_no_intercept_quick_scan.cpp`
- `projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/thread_trace/core.{hpp,cpp}`
- `projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/thread_trace/threading.cpp`
- `projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/thread_trace/tests/att_packet_test.cpp`
- `projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/thread_trace/tests/producer_consumer.cpp`
- `projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/hsa/aql_packet.{hpp,cpp}`
- `projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/hsa/queue_controller.{hpp,cpp}`
- `projects/rocprofiler-sdk/source/lib/aqlprofile/aql_profile_v2.h`
- `projects/rocprofiler-sdk/source/lib/aqlprofile/core/threadtrace.cpp`
- `projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/sqtt_builder.h`
- `projects/rocprofiler-sdk/source/lib/aqlprofile/gfxip/gfx11/gfx11_primitives.h`
- `projects/rocprof-trace-decoder/source/gfx11/quick_scan.cpp`
- `projects/rocprof-trace-decoder/source/quick_scan_export.{hpp,cpp}`
- `projects/rocprof-trace-decoder/test/unit/quick_scan_api_test.cpp`
- `projects/rocprofiler-sdk/tests/rocprofv3/launcher-att-library-path/`
- `projects/rocprofiler-sdk/tests/rocprofv3/advanced-thread-trace/CMakeLists.txt`

## Validation

### Focused build and unit coverage

```sh
cmake --build build/rocprofiler-sdk --target \
  rocprofiler-sdk-aqlprofile \
  aqlprofile-core-tests \
  aqlprofile-pm4-tests \
  thread-trace-packet-test \
  rocprofiler-sdk-tool
```

The ATT decoder used by these tests comes from `projects/rocprof-trace-decoder`; install it with `./build_trace_decoder.sh` so the in-tree library is the one `rocprofv3` loads (see Known limitations).

Set `ROCPROFILER_SQTT_DEBUG=1` to log the swap-time read-back (`STATUS`, `STATUS2`, `CTRL`, `WPTR`, `BUF0/BUF1` base and size) for a trace; it is inert otherwise.

Observed results:
- `AqlProfileV2Test.ExtendedParameterNames`: passed.
- `SqttBuilderTest.Gfx115xPrimitivesUseStatus2AndDoubleBufferBits`: passed.
- Focused parser/thread-trace SDK tests passed `43/43` (`unit.pcs_parser` 16, `unit.thread_trace` 24 including upstream's new `stop_during_cpu_backpressure` and `gpu_full_disables_queue`, `unit.pc_sampling_versioning` 3), including `unit.thread_trace.resource_deinit_drains_active_device_trace`.
- Upstream's new KFD transport tests `unit.kfd_resource*` passed `9/9`.
- The AQLprofile unit groups passed all `94` enabled tests; `SqttBuilderTest.BufferStepCalculation` remained disabled.
- Focused quick-scan tests across gfx11, gfx12, MI400, gfx9, chunk state, event mapping, and API errors passed `9/9`, including `ReportsDispatchAndEventRecordsFromGfx11Trace`.
- The full rocprof-trace-decoder tree was not used for acceptance because its 1,000+ test sweep is unsafe on this gfx1151 system.
- `thread-trace-api-*` execution fixtures: `single`, `multi`, `agent`, `agent-hsa-fallback`, `agent-before-hsa-init`, `agent-start-stop-before-hsa-init`, and `large-buffer` were run; all passed except `agent-before-hsa-init` (see Known limitations). The re-enabled upstream `triple-buffer-no-pause` fixture passed.

### Selected regions

Official fixture:

```sh
HSA_DISABLE_XDNA=1 \
ctest --test-dir build/rocprofiler-sdk -j1 --output-on-failure \
  -R 'rocprofv3-test-att-marker-trace($|\.)'
```

Result: 2/2 passed. The latest run produced a `620512`-byte ATT payload with decoded in-region data.
### No detail

```sh
HSA_DISABLE_XDNA=1 \
ctest --test-dir build/rocprofiler-sdk -j1 --output-on-failure \
  -R 'rocprofv3-test-att-no-detail($|\.)'
```

Result: execution and validation passed 2/2. The latest run produced a `247008`-byte ATT payload.

### Consecutive kernels

The CTest fixture remains disabled in project metadata, and this CTest version does not support `--run-disabled`. The registered execution and validation commands were extracted verbatim with:

```sh
ctest --test-dir build/rocprofiler-sdk -N -V \
  -R 'rocprofv3-test-att-consecutive-kernels($|\.)'
```

The registered `rocprofv3 --att --att-consecutive-kernels 8 ... vector-ops` command was run directly from the fixture working directory with `HSA_DISABLE_XDNA=1`, followed by the registered `validate.py::test_csv_data` pytest command and its original environment. The execution exited normally, wrote a `5579360`-byte ATT stream plus UI output, and the validator passed 1/1.

### Detailed triple buffer

- The five enabled API fixtures, `thread-trace-api-triple-buffer-consistency-test`, `thread-trace-api-triple-buffer-hammer-test`, `thread-trace-api-triple-buffer-slow-test`, `thread-trace-api-triple-buffer-no-pause-test`, and `thread-trace-api-triple-buffer-multiple-cmds-test`, passed `5/5` (the no-pause fixture was re-enabled upstream).
- The consistency/`NO_DETAIL` and detailed hammer fixtures generated nonempty ATT output; the detailed hammer fixture also generated decoded UI output.

### No-intercept

```sh
HSA_DISABLE_XDNA=1 \
ctest --test-dir build/rocprofiler-sdk -j1 --output-on-failure \
  -R 'rocprofv3-test-att-no-intercept'
```

Result: 3/3 once the in-tree decoder is installed (`./build_trace_decoder.sh`); with the ROCm-wheel decoder that previously shadowed it the quick scan returned no dispatch records and validation failed with `No ui_output_agent_* directories found`. It now also passes 3/3 across three consecutive fixture runs after the overflow recovery change: one to two SQTT overflows per run are staged, drained and restarted instead of ending the capture.

### Install synchronization

The final SDK build was installed with `cmake --install build/rocprofiler-sdk`, and SDK-core library links were synchronized. The rebuilt branch runtime reported version `7.16.26374-f39ab5add8` after the rebase, and `./build_trace_decoder.sh` installs the matching in-tree decoder that the ATT fixtures load. An installed `$ROCM_PATH/bin/rocprofv3` selected-regions run produced a validated ATT payload.

## Failure Protocol and GPU State

GPU-resource tests are run sequentially. After every nonzero exit, timeout, allocator abort, or crash:
- Record the command and output.
- Run `pgrep -af` for rocprofv3 and test processes.
- Inspect `dmesg --ctime`.
- Do not continue until stale processes are gone and the GPU is usable.

During queue-order experimentation, injecting queue-enable PM4 before every no-intercept dispatch caused a 47-second CTest timeout. Queue removal then failed in MES and triggered an amdgpu mode-2 reset. The reset completed successfully, no stale process remained, and `rocminfo` confirmed recovery. Replacing repeated injection with one direct activation per existing application queue before internal SQTT start restored no-intercept 3/3 and added no later reset.

## Remaining Work

- Remove the gfx11 overflow altogether rather than recovering from it: the 790,528-byte per-CU window fills in roughly 250 us while the KFD status-query/swap/copy round trip is longer. Options are to stage the retired buffer without blocking the next poll (asynchronous copy), to act on the WPTR read-back before the FULL bit is set, or to lower the trace rate for small buffers.
- Complete pending stop and drain signals when a queue does end up disabled (non-gfx11 targets still take upstream's terminal path), so teardown cannot block on a signal that will never be submitted.
- Fix the deferred single-buffer start used by `thread-trace-api-agent-before-hsa-init-test`. The queue-local start packet is dropped when it is injected into the first few submissions after queue creation, so the trace has to be verified and re-armed (or the start injection deferred until the engine accepts it) instead of assumed to have taken effect. The multi-buffer producer already has the startup-polling and restart machinery needed for a bounded retry; the single-buffer path has no status query and needs one.
- Populate `QueueThreadTraceEnableAQLPacket::before_krn_pkt` (or call `populate_before()`) so repeated queue-local enable injection is not a silent no-op.
- Add repeated/concurrent stop, start-versus-shutdown, and generation-ownership lifecycle tests.
- Move HSA waits, long device stops, and shader callbacks outside `DeviceThreadTracer::agent_mut` and the broader context mutex where applicable.
- Replace function-static consecutive-kernel callback state with a per-device-context controller.
- Add a dedicated queue-creation activation hook for multi-buffer traces so queues created after context start receive queue-local ATT state.
- Keep selected-regions plus triple-buffer mode unsupported until the empty final partial-buffer behavior is resolved.
- Add a scalar gfx11 quick-scan fallback for hosts without AVX-512.
- Harden multi-GPU agent identity, CU-bitmap/factory cache keys, and counter-dispatch cache lifetime before claiming multi-GPU support.
- Keep the public interface at explicit triple-buffer mode unless there is a demonstrated need for an arbitrary CLI buffer-count option.
