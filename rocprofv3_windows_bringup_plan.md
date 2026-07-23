# Native Windows `rocprofv3` design record

## Overview and scope

### Status

Native Windows dispatch performance-counter support is complete for the validated gfx1151 configuration. The public entry point is the ordinary installed command:

```powershell
rocprofv3 --pmc COUNTER1 COUNTER2 -- target.exe
```

The implementation uses the standard ROCProfiler SDK counter catalog, profile planner, dispatch-counting services, identities, filtering rules, and output records. It does not use a second CLR counter policy or require a special profiling runner.

The original release qualification and the later direct split-package installation, Windows/Linux availability parity, and dispatch-analysis milestones are retained in the branch history. This document contains the maintained design, operating procedures, validation baseline, intentional platform differences, settled dispatch-analysis and implementation contracts, and remaining platform boundaries.

### Supported platform and scope

The validated system is:

- native Windows;
- active Python environment `C:\venv_torch`;
- GPU `AMD Radeon(TM) 8060S Graphics`;
- canonical architecture `gfx1151`;
- ROCProfiler GPU agent/node `1`;
- 40 compute units and 80 SIMDs;
- 2 shader engines and 2 shader arrays per engine;
- 1 XCC;
- wavefront size 32;
- GFX target version `110501`.

The supported Windows subset includes:

- native agent and counter availability;
- the common 442-entry gfx1151 catalog: 228 raw counters, 155 derived metrics, and 59 agent constants;
- standard callback and buffered dispatch counting;
- raw and derived counters in the same profile;
- compatible groups, explicit multipass replay, and exact dimension decoding;
- process-global agent, queue, kernel, dispatch, and correlation identities;
- concurrent queues, reversed completion, stream teardown, and HIP graph replay;
- one-based kernel iteration filters and formatted kernel-name regular expressions;
- selected-region collection through public ROCTX marker-control callbacks, with optional reference counting;
- standard counter CSV and JSON output plus optional ROCpd 3.0.3 database output;
- bounded HIP runtime API, HIP graph, kernel activity, and ROCTX CSV tracing;
- ordinary counter plus supported HIP API trace composition in one target process;
- HIPK code-object loading from the per-architecture `.kpack` archives used by the active split PyTorch and ROCm packages;
- target-status preservation, no-replace publication, finite shutdown, and relocatable installation;
- direct replacement in the active split ROCm package without touching System32 or build-owned evidence.

The following are not part of this Windows subset:

- PC sampling;
- ATT/SQTT thread tracing;
- SPM collection;
- KFD event tracing;
- process attachment;
- HSA API tracing as a user-visible domain;
- Linux-only Perfetto/PFTrace, OTF2, DW, and related native post-processing services;
- general multi-GPU or heterogeneous-agent support;
- counter portability beyond the validated gfx1151 machine.

## Public CLI and output contract

### Agent and counter model

Windows normalizes native topology into stable ROCProfiler identities shared by availability, activity conversion, dispatch records, and counter output. The installed availability tool and SDK use the common `source/share/rocprofiler-sdk/config.yaml` catalog.

The gfx1151 contract contains:

| Kind | Count |
|---|---:|
| Raw counters | 228 |
| Derived metrics | 155 |
| Agent constants | 59 |
| Total | 442 |

Metadata includes common expressions, descriptions, required counters, dimensions, and instances. Availability dimension queries construct topology descriptors without treating them as collection requests, so `rocprofv3-avail list` and `info` do not validate every hardware event or emit profile-validation diagnostics. Counter names are sorted by the common Python frontend, giving Linux and Windows deterministic catalog ordering. `pmc-check` and normal counter configuration continue to validate the requested profile before packet construction.

The gfx11 AQL Profile descriptor bounds include the gfx115x catalog selectors used by `GRBM_GL2C_BUSY`, `GCEA_RDRAM_SIZE_REQ`, and `GCEA_WDRAM_SIZE_REQ`. Descriptors outside the qualified bounds fail normal counter-configuration validation with `ROCPROFILER_STATUS_ERROR_METRIC_NOT_VALID_FOR_AGENT`.

Profile construction uses common counter IDs, required-counter expansion, compatibility checks, deduplication, dimension decoding, and derived-AST evaluation. There is no Windows-specific grouping planner or fallback catalog.

Some native agent fields intentionally differ across operating systems. `gpu_id` is assigned by the platform topology provider and is not a cross-OS identity. Linux and D3DKMT can return different native marketing strings in `product_name`, and Linux may report `model_name` as `ip discovery` while Windows reports the normalized gfx target. Counter parity is based on the logical GPU index, gfx architecture, catalog records, dimensions, and profile behavior rather than byte-for-byte equality of those native strings.

### Counter invocation

Windows uses the common parser and pass planner. Supported ordinary examples include:

```powershell
rocprofv3 --pmc SQ_WAVES -- app.exe
rocprofv3 --pmc SQ_WAVES --pmc GRBM_COUNT -- app.exe
rocprofv3 --pmc SQ_WAVES --kernel-include-regex vector_add `
  --kernel-iteration-range "[2-3]" -- app.exe
rocprofv3 --pmc SQ_WAVES --selected-regions -- app.exe
rocprofv3 --pmc SQ_WAVES -f rocpd -- app.exe
rocprofv3 --pmc SQ_WAVES --hip-runtime-trace -f csv json -- app.exe
```

Multiple `--pmc` groups replay the normalized target command and publish under `pass_1`, `pass_2`, and subsequent pass directories. Kernel iteration ranges are one-based. Kernel-name filters apply to the formatted name after `.kd` normalization and COMGR demangling.

The Windows LLP64 signature is retained accurately: `size_t` demangles as `unsigned long long`; Linux uses `unsigned long`.

Selected-region collection uses ordinary context start/stop through public marker-control callbacks. Without a resume interval it emits no counter output. Nested intervals use either non-reference-counted or reference-counted semantics according to the common option.

Unknown counter names warn with `Unable to find counter`, produce no counter output, and preserve the target status. A statically incompatible profile fails through normal profile validation. A target with no dispatches produces no counter files. A nonzero target status is returned unchanged when no profiler error takes precedence.

On the current gfx1151 WDDM path, `CP_PERFMON_CNTL` must use the hardware's always-count mode. The context-gated mode produces zero because WDDM does not establish the CP performance-counter context used by that mode. Counter values therefore describe GPU activity while the selected dispatch window is open and can include concurrent GPU work; an exact `SQ_WAVES` total is not a valid live-hardware invariant. Another overlapping performance-counter session can also reset or stop the GPU-wide counters. The tool rejects a nonpositive aggregate `SQ_WAVES` sample as `counter_sample_invalid` instead of publishing it as a successful record. Qualification requires stable record shape, valid dispatch identities, and positive values, without claiming cross-process counter isolation that the WDDM runtime and driver do not provide.

Counter `--stats` does not invent counter statistics. Summaries remain dependent on supported timed trace domains.

### Tool discovery and environment

The child uses the common `ROCPROF_COUNTERS`, `ROCPROF_COUNTER_GROUPS`, and `ROCPROF_*` configuration contract. The launcher resolves the SDK, counter tool, availability tool, COMGR, metrics, registration library, and runtime relative to the selected build or installed prefix.

`ROCP_TOOL_LIBRARIES` identifies the standard counter tool. Windows loads it with `LoadLibraryA`, resolves `rocprofiler_configure`, and configures the client outside registration locks.

Inherited HSA-tool and unsupported PM4 activation variables are removed from normal children. The launcher starts the requested executable at its original resolved path and prepends the selected runtime/tool directories to `PATH`; it does not copy or rewrite the application image. Application-local DLL and resource behavior is therefore preserved.

### Process containment and publication

`_rocprofv3_windows_job.py` creates the root child suspended, assigns it to a kill-on-close job, reserves every expected output, resumes it, applies finite normal and termination waits, and returns durable timing and exit metadata.

The frontend resolves one effective output and trace plan before the Windows/Linux backend split. Explicit false options remain authoritative, static output keys are normalized once, and target-PID-dependent keys are expanded after suspended process creation from the same declared rules used by the native producer. Expected output names include an inherited `ROCPROF_OUTPUT_FILE_NAME` and `%pid%`, `{pid}`, `%cwd%`, and `%hostname%` expansion. The resulting nested base path is shared by kernel, API, counter, statistics, JSON, ROCpd, and multipass destinations.

All outputs are reserved before target side effects. One ownership-aware transaction tracks the exact reservations and profiler-created files across standalone activity, API/marker traces, composed SDK output, and post-target ROCpd conversion. Existing destinations cause failure rather than replacement; target-created conflicts remain untouched, while profiler-created partial outputs and temporary conversion artifacts are removed when a later validation or publication step fails.

### Trace and output contract

The current bounded Windows trace selectors include HIP runtime APIs, the supported HIP graph model, kernel activity, and ROCTX marker APIs and completed ranges. Trace requests require CSV. A composed kernel-trace and counter run uses one authoritative dispatch-counting record for both artifacts and serializes those kernel records into the SDK counter JSON document.

Counter collection publishes:

- the standard 19-column `counter_collection.csv` schema;
- the standard 53-column `agent_info.csv` schema;
- the Linux-compatible 22-column `kernel_trace.csv` schema when composed kernel tracing is requested;
- the Linux-compatible eight-column `kernel_stats.csv` schema when statistics are requested;
- JSON rooted at `rocprofiler-sdk-tool`, with records under `callback_records.counter_collection`, composed records under `buffer_records.kernel_dispatch`, descriptor snapshots under `kernel_symbols`, and selected-kernel accumulators under `summary`;
- an optional `*_results.db` ROCpd 3.0.3 database with the upstream tables, views, metadata, dimensional PMC events, dispatch/resource identities, and summary views.

Composed kernel tracing and counter collection run in one target process. Composed CSV/JSON/ROCpd records share the SDK dispatch identity and authoritative system-domain timestamps. HIP runtime API tracing can run in that process as a separate CSV output. ROCpd-only requests use an internal JSON conversion artifact after target exit and remove it after successful conversion; an explicitly requested JSON artifact remains. Publication is exclusive and does not replace existing files.

The Windows-minimal backend rejects zero or unordered counter timestamps and publishes one timed composed kernel record per selected dispatch. Kernel-resource fields come from symbol and translated-descriptor snapshots; missing mandatory metadata is `kernel_metadata_missing`, not a zero-valued measurement. Standalone CLR activity carries its own authoritative per-operation resource snapshot and is never heuristically joined to SDK records.

## Dispatch analysis contract

### Canonical workflow

The completed installed workflow is:

```powershell
rocprofv3 --kernel-trace --stats `
  --pmc L2CacheHit VALUInsts LDSBankConflict `
  --kernel-include-regex <kernel_regex> `
  -f csv json rocpd `
  -d <out_dir> -o <prefix> -- python <script>.py
```

It preserves the 40-export Windows-minimal plain-C API boundary, target status, checked private results, no-replace publication, and direct split-package layout. The repository-owned dispatch-analysis workload and declarative behavior matrix define the maintained acceptance contract; FeatherOps remains an external case study.

### Behavioral oracle

`tests/bin/dispatch-analysis/dispatch_analysis.cpp` supplies a same-source six-dispatch, two-stream fixture with three repeated vector launches, two LDS-conflict launches, a resource-heavy launch, reversed-completion, no-dispatch, and target-failure modes. `tests/dispatch-analysis/behavior_cases.json` is the shared declarative matrix for formatted-name selection, iteration filtering, composition, statistics, failure behavior, and publication, and its CPU-only contract test is registered in the Windows graph.

The same fixture was compiled and run on `x2` after sourcing `~/rocm.sh` and activating `~/venv_torch`. The retained oracle in `tests/dispatch-analysis/gfx1151_linux_oracle.json` freezes the Linux CSV columns and shapes, JSON nodes, statistics schema, positive timing/counter requirements, descriptor resource shape, and `kernels`/`top_kernels` ROCpd views. The Linux and Windows tools now apply one cached one-based kernel selection to counter, trace, statistics, JSON, and ROCpd records by authoritative dispatch ID. Linux remains the schema, naming, timing, resource, formula, and database oracle except where a focused cross-platform test identifies an existing Linux behavior as defective; in that case the implementation and retained oracle must be corrected together.

### Authoritative timing

The missing path was the SDK wrapper around an existing HIP queue: newly created intercept queues enabled profiling, while wrapped queues did not. Both constructors now require `hsa_amd_profiling_set_profiler_enabled`, and completion handling calls the common `kernel_dispatch::get_dispatch_time` on the profiler-owned replacement completion signal before pool release. Successful HSA calls with zero or unordered raw timestamps are rejected; the Windows-minimal branch preserves ROCr's already translated system-domain nanoseconds without applying the Linux KFD clock-skew adjustment. No CLR fallback or heuristic correlation is used.

ROCr now routes `Queue::SetProfiling` and WDDM's `EnableProfiling` through one shared `amd_queue_v2_t` property helper. CPU tests verify enable/disable propagation and that enabled kernel, barrier, and vendor-packet timestamp commands select the completion signal's `start_ts` and `end_ts` fields. `projects/rocr-runtime/scripts/build_windows.ps1` rebuilt and installed the runtime after all four component tests passed. The existing independent WDDM epoch-alignment tests remain the clock-domain translation gate.

The focused SDK timestamp gate passed all six selected CTests: the three-case contract, nine PMC CLI cases, and callback, buffered, concurrent, and grouped dispatch-counting modes. The service tests reported valid timing for all 4 callback, 4 buffered, 7 concurrent, and 8 grouped records, including reversed completion and three concurrent queues. The full build then passed all 20 process-level execute/validate tests, build-prefix validation, and installed-prefix HIP/PMC validation. Ten fresh installed PMC processes each published two timed CSV and JSON records. The six-dispatch composed fixture published 18 counter rows with six unique intervals, start timestamps at or above `224615860672652` ns, end timestamps at or below `224615863041442` ns, and durations from `34,346` through `71,456` ns. The common dispatch-time source also rebuilt on `x2`; after prefix installation, two Linux PMC runs retained the stable 32-row shape, unique dispatch IDs, positive `SQ_WAVES`, and prefix-local SDK/tool library resolution.

### Composed records and publication

The Windows tool uses one SDK-dispatch-ID ordering helper for stored `rocprofiler_dispatch_counting_service_data_t` records and derives counter, kernel, statistics, and JSON artifacts from those objects. Kernel CSV uses the Linux 22-column order, while JSON places the common extended dispatch records under `buffer_records.kernel_dispatch` and counters under `callback_records.counter_collection`. Finalization rejects duplicate dispatch IDs, requires selected/completed/emitted parity, and publishes every requested output through the exclusive no-replace transaction. Each explicit PMC replay remains a separate process with process-local IDs and complete pass-local artifacts.

### Selection and standalone activity

The standalone trace remains an intentional Windows specialization: it uses CLR's activity source and its stream/correlation representation, while the composed path uses SDK dispatch identity. Only composed artifacts claim dispatch-level joinability with PMCs.

`kernel_selector.hpp` owns the shared composed formatted-name decision, uses `lib/common/regex` plus `counter_config_common.hpp`, counts one-based iterations per final formatted name, and caches each result by SDK dispatch ID. The dispatch callback invokes it while the dispatch-counting context is active and before selecting a counter profile, so completion order and later CSV, JSON, statistics, or ROCpd consumers cannot advance selector state. Neither platform maintains a second selector implementation.

CLR activity dispatch records now carry an explicit API enqueue ordinal, a one-based operation index for multi-operation records, the enqueue thread ID, and the mangled kernel name. The standalone converter requires those fields, rejects invalid or duplicate ordinal pairs, sorts by `(enqueue_ordinal, enqueue_operation_index)`, formats the name before filtering, and counts iterations per formatted name. Its local dispatch sequence remains standalone activity identity; it is not correlated heuristically with SDK counter records. The shared behavior contract is version 3 and includes mangled and truncated name cases across no-filter, include, exclude, include-plus-exclude, nonmatching, exact/range iteration, and reversed-completion cases.

The rebuilt CLR passed its binary and normal/late registration tests, direct `rocm_kpack.dll` dependency audit, active devel/core publication, and a real installed Torch GPU/event-timing probe. The SDK pre-install gate passed 13 of 13 CTests, including 12 standalone converter unit cases, the 3-case dispatch contract, the 6-case PMC contract, all registration/process tests, and 11 live PMC process cases. All 20 safe execute/validate integrations then passed, followed by the 3-case build-prefix test and both installed-prefix tests. The ten-case dispatch matrix passed again through the installed devel-package launcher: standalone and composed selected exactly the declared enqueue sets, reversed completion selected the second `dispatch_vector` at ordinal 3, mangled/truncated output used the requested final names, and the nonmatching case returned target status zero with no selected rows. Composed nonmatching publication produced no counter, trace, or JSON file; standalone publication contained only its CSV schema and no data rows. Four malformed-regex/range process checks returned profiler status 1 with no output files. Post-install checks retained 40 plain-C SDK exports, no package-owned validation/provenance directories, no matching fixture process by image or exact executable path, and an unchanged System32 HIP runtime.

### Statistics

The accumulator, map, entry, and descending-total sorter now live in dependency-neutral `source/lib/output/statistics_data.hpp`; the full Linux output layer includes that header and retains its domain and presentation types in `statistics.hpp`. Windows-minimal compiles the existing `statistics.cpp` formatter and uses the shared accumulator over the authoritative selected dispatch records. Both composed and standalone paths group by the final formatted kernel name, use positive ordered nanosecond intervals, emit the exact eight Linux CSV columns, use sample standard deviation, and suppress statistics when selection is empty. The composed JSON `summary` has one `KERNEL_DISPATCH` domain and matches Linux's Cereal representation, including total accumulator fields, duration-ordered `operations` key/value entries with deterministic name tie-breaking, and class-version markers. No counter summary is generated.

Statistics destinations are reserved before target side effects and participate in the existing exclusive publication and owned rollback transaction. A target-created statistics conflict is retained while earlier profiler-created agent, counter, and trace files are removed. Multipass collection publishes a complete statistics file in each process-local pass directory. Invalid selected timing and missing formatted metadata are explicit profiler failures. Standalone conversion reserves its trace and statistics siblings together, computes statistics only after formatted-name filtering, removes its own trace if later statistics publication fails, and leaves no statistics artifact for a successful empty selection.

Maintained CPU tests check exact count, sum, mean, percentage, min, max, sample standard deviation, and tie ordering. The live process suite covers composed CSV/JSON statistics, pass-local multipass summaries, no-dispatch behavior, target-created statistics conflicts, and cleanup; these cases are included in the 42-test Windows graph and the installed dispatch matrix.

A direct installed FeatherOps command over `L2CacheHit`, `VALUInsts`, and `LDSBankConflict` emitted six ordered kernel rows, 18 per-dispatch counter rows, three descending-total kernel-statistics rows with call counts `3`, `2`, and `1`, six kernel JSON records, and a JSON summary with total count six and three operations. Validators recomputed every CSV and JSON accumulator from the trace rows; the percentage total was within `0.02` of 100. Post-install checks found no validation/provenance package directories or matching fixture process, retained 40 plain-C SDK exports, and left the System32 HIP runtime unchanged.

On `x2`, after sourcing `~/rocm.sh` and activating `~/venv_torch/bin/activate`, the exact final source completed a full non-Windows-minimal Linux build and prefix installation. Two prefix-local runs of the six-dispatch fixture each emitted six kernel rows, 18 counter rows, and three statistics rows; all three counter sums were positive, the shape and per-name call counts were stable, and recomputation validated the Linux CSV formulas and JSON summary. `LD_DEBUG=libs` confirmed that both `librocprofiler-sdk.so` and `librocprofiler-sdk-tool.so` resolved from the qualification prefix.

### Resource and occupancy metadata

Windows may report `code_object_id=0`, `stream_id=0`, and graph IDs of zero when the bounded HSA path has no stable source for those optional associations. Queue, kernel, correlation, and dispatch identity plus timing and resource fields remain mandatory.

`code_object/kernel_descriptor.hpp` now owns the exact 64-byte descriptor layout, gfx architecture parsing, SGPR/VGPR/accumulated-VGPR decoding, and checked positive or negative entry-address arithmetic. The Linux code-object path uses this helper and no longer falls back to dereferencing a device-valued kernel object when AMD loader translation fails. The Windows symbol observer initializes `HSA_EXTENSION_AMD_LOADER`, queries name, kernarg, group/private segment, and agent architecture attributes while the symbol is valid, translates the kernel object with `hsa_ven_amd_loader_query_host_address`, copies the descriptor, and registers a synchronized snapshot keyed by kernel object and SDK kernel ID. A later invalid observation cannot replace a valid snapshot.

The Windows tool requires a valid snapshot before selecting a profile and publishes `kernel_metadata_missing` with the snapshot error when mandatory metadata is unavailable. Composed counter CSV, kernel CSV, JSON dispatch records, and JSON `kernel_symbols` consume the same snapshot; LDS uses 512-byte block normalization and completed JSON records receive the snapshotted group/private sizes. No public tracing API was added and the installed SDK still exports the audited 40 plain-C functions.

CLR activity records now carry group/private segment, architecture-VGPR, accumulated-VGPR, SGPR, and validity fields through ordinary dispatches, captured graph nodes, and spill records. `hipGpuActivityExt` remains 128 bytes, uses 24 bytes of prior reserved padding, and advances `HIP_PROFILER_EXT_VERSION_MINOR` from 1 to 2. On gfx10 and later, standalone activity obtains group/private sizes and VGPR usage from the CLR device kernel, applies wave-granularity VGPR allocation, reports zero accumulated VGPRs where inapplicable, and reports the architectural 128-SGPR allocation. The standalone converter rejects missing or invalid fields as `kernel_metadata_missing` and performs no SDK join.

The six-dispatch contract now includes a dynamically indexed private-array kernel and freezes platform-specific gfx1151 metadata. Windows standalone and composed output both report vector/LDS/resource kernels as LDS `0/8192/4096`, scratch `0/0/132`, VGPR `8/8/24`, accumulated VGPR `0/0/0`, and SGPR `128/128/128`. Linux reports the same LDS/register values and an authoritative resource-kernel scratch size of `144`; that compiler/ABI difference is explicit rather than normalized. Windows validation checks every selected standalone, trace, counter, JSON dispatch, and JSON symbol record, and focused negative tests require `kernel_metadata_missing` for invalid CLR resource data.

The final Windows CTest graph passed 42 of 42 tests, including the shared nine-case descriptor coverage, all 20 execute/validate integrations, the 12-case fresh-process PMC suite, build-prefix checks, and installed CPU/GPU qualification. A direct installed-wrapper run emitted six standalone rows, six composed rows, 18 counters, three statistics rows, six JSON dispatch records, and three workload symbols with exact resource parity. Build, devel, and core SDK/tool copies are byte-identical; the rebuilt CLR DLL agrees with its installed copy; installed packages contain no `validation` or `provenance`; no fixture process remained by image or exact path; and the System32 HIP runtime remained unchanged.

### ROCpd analysis

`_rocprofv3_rocpd.py` is a post-target converter over the Windows tool's authoritative SDK JSON. It loads the installed upstream `rocpd_tables.sql`, `rocpd_views.sql`, `data_views.sql`, `summary_views.sql`, indexes, and metadata, substitutes schema identifiers through structured inputs, and writes the Linux ROCpd 3.0.3 process, node, agent, thread, queue, stream, symbol, dispatch, and PMC relations. Dispatch timing, names, IDs, dimensions, and resources come from the composed JSON records; conversion does not implement a second selector, clock, identity assignment, or descriptor decoder. Raw PMC instances remain distinct rows, while derived counters remain one event per authoritative dispatch value.

`versions.yml` is the maintained schema authority. Installation generates the latest-schema manifest from its semantic version and six asset mappings; the converter validates that manifest and derives schema substitutions, diagnostics, and SQLite `user_version` instead of repeating version literals. Missing, repeated, escaping, or inconsistent schema assets fail conversion before publication. Native CSV and ROCpd resource rows use the shared 512-byte LDS allocation helper, while the Python converter applies the equivalent checked normalization.

`rocpd` participates in normal Windows PMC format planning and uses the standard `*_results.db` name. A ROCpd-only request forces SDK JSON as a private intermediate, reserves both paths before target launch, converts only after successful target/profiler completion, and removes the JSON unless the user requested it. Mixed CSV, JSON, and ROCpd requests retain every explicit primary artifact. Database construction uses an adjacent temporary file, `PRAGMA foreign_key_check`, `PRAGMA integrity_check`, `user_version=30003`, and no-replace hard-link publication. Conversion and publication errors are reported as `rocpd_conversion_failed`; existing databases and target-created conflicts are preserved, while only profiler-created temporary/internal artifacts are removed.

The converter and current/versioned SQL assets install through the Windows tools component into both direct alternate prefixes and the active devel package. The SDK remains configured with `ROCPROFILER_BUILD_SQLITE3=OFF`; no SQLite DLL, import library, validation directory, or provenance directory is installed. The build, devel, and core SDK/tool copies are byte-identical, the installed converter/schema assets are required by prefix validation, the 40 plain-C export count is unchanged, and the System32 HIP runtime remains unchanged.

Windows CPU conversion tests query schema metadata, `kernels`, dimensional `pmc_events`, `kernel_symbols`, `top_kernels`, integrity, and foreign keys. Live tests cover ROCpd-only collection, mixed trace/PMC formats, conflicts before and after target launch, conversion failure cleanup, and process-local multipass databases. The complete Windows graph passed 42 of 42 CTests, including all 20 execute/validate integrations and the fresh-process PMC suite. Installed-prefix HIP ROCpd emitted two kernels and 40 positive dimensional `SQ_WAVES` events. The direct six-dispatch wrapper qualification emitted six kernels, 18 derived-counter events, three symbols, and three summary rows with exact dispatch/resource identity and schema `3.0.3`; all integrity and foreign-key checks passed.

The qualified source archive was transferred to `x2`, configured cleanly with `ROCPROFILER_BUILD_WINDOWS_MINIMAL=OFF` and tests enabled, built, and installed under `/home/wd/build-rocm-systems-step6/prefix`. The three shared behavior-contract tests and all five descriptor GTests passed. Two prefix-local gfx1151 CSV/JSON runs each produced the stable six-dispatch, 18-counter, three-statistics, three-symbol shape with positive counter sums and exact Linux resources. A native Linux ROCpd run produced six `kernels`, 18 `pmc_events`, three workload `kernel_symbols`, and three `top_kernels` rows using schema `3.0.3`; timing/resource joins, integrity, and foreign keys passed. `LD_DEBUG=libs` resolved both SDK and tool from the qualification prefix, and the installed converter was byte-identical to the qualified source.

## Architecture and invariants

### System invariants

#### Execution authority

HIP/CLR is the sole kernel-execution authority for accepted workloads. HIP owns code-object loading, hidden kernargs, memory allocation and mapping, streams, graphs, kernel launch, synchronization, and result validation. ROCProfiler observes or wraps producer-owned API tables and queues; it does not reconstruct application kernels through direct HSA.

A bounded direct-HSA test may submit one finite barrier packet to prove registration and queue forwarding. Direct-HSA kernel production is not part of the product or normal test graph.

#### Public ABI and runtime capability

The public HSA vendor-packet ABI remains 64 bytes. WDDM command construction metadata, allocation tracking, and validation remain private implementation details. The metadata follows the existing ROCm producer-process trust boundary; this port does not introduce a new authentication boundary between code running in the same process.

`ROCR_USE_PM4` and `WSLKMT_VENDOR_PACKET` are not activation inputs. ROCr/libhsakmt reports the validated WDDM AQL Profile capability from discovered runtime state. Unsupported capability or malformed profile requests return normal errors before hardware submission.

The active ROCm and PyTorch installation uses host/device split packaging. Its host binaries carry HIPK markers rather than embedded device code, so the installed HIP/CLR runtime must be built with `ROCM_KPACK_ENABLED=ON` and must resolve the package-owned `rocm_kpack.dll`. A runtime built with this option disabled can enumerate and allocate on the GPU but rejects the first HIPK kernel launch with `hipErrorUnknown`; it is not compatible with this installation model.

#### Protected runtime policy

Ordinary builds use the package-owned build directory and install their component directly into `ROCM_PATH`. Build-tree component tests run before installation, while tests that verify the active prefix run against the resulting installed files. Replacement is allowed and deliberately overwrites the current package version.

The installation helper removes each destination before replacement and uses a temporary sibling file, so package-cache hard links are not modified in place. No build mode targets System32; the HIP runtime script records and verifies that System32 remains unchanged.

#### Provenance

The only source-pinned binary input is:

```text
C:\rocm-systems\shared\amdgpu-windows-interop\wkmi\win\lib\wkmi.lib
size: 448252
MD5:  d5d5d50aa73f85886029e3dcbce7f03f
```

Build wrappers fail closed when this archive does not match. Generated binaries, installed packages, and system libraries are validated dynamically; their changing hashes are not recorded in this document.

### Component architecture

#### HIP/CLR and ROCr

The Windows HIP runtime contains embedded ROCr and supplies the ordinary mutable HIP and HSA API tables and queue boundary. It links the standard kpack runtime to resolve HIPK code objects from package-relative, architecture-specific archives. CLR does not parse counter names, select dispatches, load or decode private counter profiles, claim a process-wide queue, or write counter files.

ROCr/libhsakmt owns WDDM queue submission and vendor-packet translation. The WDDM boundary validates command identity, checksum, packet lengths, opcodes, registers, event descriptors, instance masks, result addresses, allocation bounds, completion-signal ownership, and START/READ/STOP ordering before emitting an indirect-buffer jump.

#### AQL Profile

AQL Profile owns gfx1151 event metadata, profile compatibility, resource sizing, packet construction, and counter decoding. Standalone and SDK-embedded builds use the same generated interfaces and private WDDM construction contract.

The qualified gfx1151 software-queue frame is `0x2000` bytes. A `0x100`-byte trailer remains reserved, and reported usable PM4 capacity is capped at 1984 dwords. The minimum `0x500` capability case remains covered by the adapter tests.

Command, result, and supporting allocations remain live through the GPU fence. Completion signals are duplicated or retained as required, racing frees are deferred, and queue teardown drains pending submissions before releasing resources.

#### `rocprofiler-register`

Windows registration uses PE-native module discovery, `LoadLibraryW`, `GetProcAddress`, `GetModuleFileNameW`, normalized paths, and loaded-image address validation. Secure mode accepts a producer import function only when its address belongs to an allowed loaded module.

Registration preserves mutable producer-owned tables and supports normal and retained-late startup. Tool discovery, `rocprofiler_configure`, initialization, and API-table callbacks run outside the registration mutex. Reentrant propagation is tracked explicitly so an API table is not replayed while its first propagation is still in flight.

Repeated table propagation is idempotent. In particular, the HSA symbol-info wrapper does not capture itself as its downstream function.

#### ROCProfiler SDK

`ROCPROFILER_BUILD_WINDOWS_MINIMAL=ON` includes the common context, buffer, counter configuration, metric evaluation, dispatch handling, sample processing, and correlation layers needed by the supported Windows services. Linux-only service implementations remain excluded.

The SDK captures the original HSA core and AMD-extension tables and initializes its GPU agent cache and queue controller at the first reliable queue boundary. Because Windows HIP/CLR can reach `hsa_amd_queue_create` before `hsa_queue_create`, the AMD-extension entry is wrapped lazily so either queue path installs dispatch interception while preserving the original producer function. Windows-specific code is limited to PE registration, native topology, HSA table adaptation, allocation, and WDDM submission mechanisms.

The generated Windows-minimal graph is audited as part of the service boundary. The HSA PC-sampling adapter translation unit and header belong only to the non-minimal source set; the Windows-minimal graph has no PC-sampling, KFD, firmware, ATT, SPM, Perfetto, OTF2, DW, SQLite, or pthread service objects. The non-minimal Linux graph continues to compile the PC-sampling adapter, and the live AQL, WDDM, registration, availability, standalone CLR, composed SDK, dispatch-analysis, and synthetic ROCpd qualification paths remain retained.

#### ROCTX

The Windows ROCTX producer supplies mutable tables for marks, nested thread ranges, explicit process ranges, pause, and resume. The SDK preserves downstream results and emits paired API records plus completed semantic marker/range records.

Pause and resume are delivered through `ROCPROFILER_CALLBACK_TRACING_MARKER_CONTROL_API`. Selected-region state transitions and the corresponding context start/stop calls are serialized so concurrent callbacks cannot reorder them.

### Dispatch-counting service

Both public delivery modes are supported:

- `rocprofiler_configure_callback_dispatch_counting_service` delivers standard `rocprofiler_counter_record_t` records;
- `rocprofiler_configure_buffer_dispatch_counting_service` emits standard dispatch headers and value records through an SDK buffer and `rocprofiler_flush_buffer`.

Each instrumented dispatch owns its agent, queue, kernel, dispatch, internal correlation, external correlation, profile selection, packet state, signal state, and completion session. Profile-selection `rocprofiler_user_data_t` is copied into every public record.

Queue and dispatch IDs are process-global. Kernel IDs are derived from stable HSA executable symbols. Windows observes executable-symbol iteration and direct symbol-info queries because HIP uses both paths.

Only dispatches with actual profiling instrumentation enter the common profiler serializer. This serializes real AQL Profile resource conflicts without unnecessarily serializing unrelated process work. Completion releases correlation references, kernel references, profile resources, and pooled signals at the per-dispatch boundary.

Concurrent validation covers multiple nonblocking streams, reversed completion, graph replay, and queue creation/destruction. Grouped validation covers raw and derived metrics together, multi-block profiles, exact instances, deterministic pass membership, and repeated graph replay.

### Lifecycle and shutdown

The first accepted producer table initializes the SDK once. Normal teardown finalizes each client once and records exactly one initialize/finalize lifecycle pair for the bounded trace backend.

Windows HSA shutdown finalizes SDK clients before HSA resources are torn down. Client contexts are stopped and the queue controller is synchronized while queue workers, HSA signals, and queue infrastructure are still live. Only after dispatch callbacks are drained does the tool finalizer serialize records. Queue-controller, correlation, and internal-threading teardown follows serialization.

The callback consumer moves its worker thread out while holding the consumer mutex and joins after releasing that mutex, avoiding a shutdown join against a worker waiting for the same lock.

## Windows/Linux parity and boundaries

### Deliberate Windows differences

Windows and Linux share the public counter catalog, profile planning, SDK dispatch-counting APIs, filtering semantics, identities, and counter output contract. The following implementation and platform differences are intentional.

#### Submission and runtime trust model

Linux submits through its established HSA/KFD runtime path. Native Windows uses the qualified WDDM software AQL-to-PM4 queue because the Linux KFD path is unavailable. The Windows thunk therefore carries private packet metadata, retains referenced allocations and signals, validates the expected command shape, and translates accepted packets into the WDDM command frame.

The private manifest and checksum follow the same producer-process trust model as the existing Linux user-mode runtime. They are integrity and compatibility metadata, not authentication credentials against hostile code running in the same process. Changing the ROCm runtime or driver security model is outside the scope of this Windows port and outside the consistency work below.

#### Registration and dynamic loading

Linux uses ELF discovery, `dlopen`, and its existing interposition infrastructure. Windows uses PE module discovery, `LoadLibrary`, `GetProcAddress`, mutable producer API tables, and explicit retained-late propagation through `rocprofiler-register`. The current ordinary CLI configures one standard counter tool from `ROCP_TOOL_LIBRARIES`. The registration layer nevertheless supports multiple independently initialized clients, retained-late propagation, reentrancy, and exactly-once finalization; those behaviors are covered by the qualified registration tests.

#### Supported SDK and tool services

Linux builds the full SDK tool and its optional tracing and output services. `ROCPROFILER_BUILD_WINDOWS_MINIMAL=ON` deliberately builds only the common SDK layers needed by availability, dispatch counting, selected regions, and the bounded HIP/ROCTX trace subset. Windows has a dependency-light native tool implementation so unsupported Perfetto, OTF2, DW, KFD, ATT, SPM, PC-sampling, and native SQLite code is not pulled into the Windows-minimal C++ graph.

Counter CSV and JSON retain the common schema. Optional ROCpd output is generated after target exit from the same authoritative SDK JSON records by a packaged Python `sqlite3` converter using the upstream ROCpd 3.0.3 tables and views. Windows HIP and ROCTX trace requests require CSV, and trace rows are not added to counter JSON. Kernel activity conversion uses the native HIP activity boundary because the full Linux tracing pipeline is not available.

#### Agent and availability data

Linux derives topology through its native discovery stack; Windows derives it from D3DKMT and the HSA tables exposed by the Windows runtime. `gpu_id`, `product_name`, and `model_name` are platform-native fields and may differ. Logical GPU index, gfx architecture, counter records, dimensions, expressions, and profile behavior remain the parity contract.

The Windows availability DLL intentionally avoids linking the Linux-only metadata and output dependency graph. It still calls the ordinary SDK availability APIs and feeds the common Python frontend. Counter ordering and PMC list, info, and check output are required to match same-source Linux output after line-ending normalization.

Windows installs `rocprofv3-list-avail.dll` directly in `bin` beside `rocprofiler-sdk.dll`. The Linux `lib/rocprofiler-sdk` layout relies on RPATH and is not reused for the PE runtime: a nested Windows DLL cannot resolve its parent-directory SDK dependency during package-level direct-load tests. Installation removes the obsolete nested copy from both split packages. This direct placement, independent split-package replacement, `.cmd` forwarding wrappers, suspended Job Object launch, and System32 exclusion are deliberate Windows packaging and process-model differences, not alternate profiling semantics.

#### Process and installation model

Linux launches through the normal POSIX process and loader model. Windows resolves and launches the original `.exe` in a suspended kill-on-close Job Object, prepends selected DLL directories to `PATH`, and preserves application-local DLL and resource lookup.

Linux packages can use links between package namespaces. Windows replaces package-owned files independently in `_rocm_sdk_devel` and `_rocm_sdk_core` so package-cache hard links are broken before publication. Windows also installs `.cmd` forwarding wrappers and never publishes to System32.

#### ABI spelling and optional fields

Windows follows LLP64, so `size_t` demangles as `unsigned long long`; Linux uses `unsigned long`. The Windows-minimal backend requires positive ordered system-domain dispatch timestamps and descriptor-derived kernel-resource metadata. Zero remains valid only for an architecturally inapplicable field or a dispatch that genuinely uses no such resource; missing mandatory metadata fails as `kernel_metadata_missing`.

#### ROCpd host and code-object metadata

Linux ROCpd records native Linux node, process, command-line, GUID, and code-object observations. The Windows post-target converter records the equivalent relational process and host roles using Windows system data, Windows command-line quoting, and a converter-generated GUID. Those values are platform-native provenance rather than cross-OS equality fields; schema version, process-local dispatch identity, timestamps, dimensions, resources, counters, and view semantics remain the parity contract.

The bounded Windows tool intentionally emits an empty JSON `code_objects` collection when it has no authoritative code-object relation, even though its translated kernel-descriptor snapshots remain valid. Because the upstream ROCpd schema requires every kernel symbol to reference a code-object row, the converter creates a synthetic row for each referenced code-object ID and marks its `extdata` with `windows_placeholder`. Its empty URI and zero load range are not inspectable code-object metadata and must not be treated as such. This bridge is required for current schema integrity until Windows captures authoritative code-object records; it is distinct from mandatory kernel resource metadata, for which placeholders remain prohibited.

### Dispatch-analysis parity summary

| Contract | Reused Linux behavior | Intentional Windows behavior |
|---|---|---|
| Timing | System-domain nanoseconds, ordered start/end validation, and one duration per dispatch. | Timestamps originate from the profiler-owned completion signal translated by the WDDM ROCr path rather than KFD. CLR timing remains only the standalone trace source or an explicitly tokenized fallback. |
| Composed identity | SDK agent, queue, kernel, correlation, and dispatch IDs are the artifact join contract. | `(Process_Id, Dispatch_Id)` is process-local; `stream_id`, graph IDs, and `code_object_id` may remain zero when the bounded Windows services have no stable source. |
| Filtering | Final formatted kernel name, common include/exclude semantics, and one-based per-kernel enqueue iteration. | Composed filtering runs in the SDK tool; standalone filtering consumes an explicit CLR enqueue ordinal and does not claim PMC joinability. |
| CSV, JSON, and statistics | Linux column names, nanosecond units, kernel summary formulas, and JSON node/schema names. | Perfetto/PFTrace and OTF2 remain absent; PE/LLP64 name spelling remains platform-native. |
| Resource metadata | HSA symbol attributes, shared descriptor decode, 512-byte LDS rounding, and per-dispatch scratch semantics. | Optional code-object associations may be absent, but timing, LDS, scratch, VGPR, accumulated-VGPR applicability, and SGPR fields may not be placeholders. |
| Database analysis | Versioned ROCpd 3.0.3 tables, views, metadata, dimensional PMC rows, dispatch identities, resources, and summaries. | Windows uses a post-target Python `sqlite3` converter outside `ROCPROFILER_BUILD_WINDOWS_MINIMAL`; Linux uses the native generator. Both publish the standard `*_results.db` contract. |
| Process and publication | Target status and output schema remain tool contracts. | Windows retains original-image launch, a kill-on-close Job Object, prelaunch sibling reservations, exclusive Win32 publication, split-package placement, and exact-path cleanup checks. |
| Public API boundary | Dispatch counting uses the ordinary ROCProfiler SDK data record. | The acceptance workflow adds no public kernel-tracing API and retains the audited 40 plain-C Windows-minimal exports. |

### Unsupported and deferred features

Completed dispatch analysis is not a prerequisite task for the areas below. They remain separate platform-breadth projects and must preserve the qualified dispatch-counting, output, package, and API boundaries.

#### Deferred hardware profiling

| Area | Current Windows boundary |
|---|---|
| PC sampling | Built with `PC_SAMPLING_SUPPORT=OFF`; user-visible configuration discovery, host-trap and stochastic collection, PC decoding, and sample output are absent. |
| ATT/SQTT | Capture, decoder integration, occupancy/timeline artifacts, instruction statistics, and gfx115x triple-buffer handling are absent. |
| SPM | Streaming performance-monitor collection is not implemented for Windows. |

#### Other platform breadth

| Area | Current Windows boundary |
|---|---|
| HIP APIs | Only the bounded runtime subset used by current HIP, graph, and marker tests is wrapped; compiler API tracing is absent. |
| Graphs | Tracking is limited to create, kernel-node, instantiate, launch, and destroy with conservative launch records. |
| ROCTX | Marks, nested thread ranges, explicit process ranges, and selected-region pause/resume are supported; kernel renaming and broader ROCTX tooling are absent. |
| Memory activity | HIP allocation/copy API rows may exist, but no equivalent memory-copy, allocation, or scratch activity domain is emitted. |
| HSA APIs | HSA tables support registration and queue interposition, not a user-visible `--hsa-trace` domain. |
| Targets and agents | A resolved native `.exe` and the validated CPU-node-0/gfx1151-node-1 model are required. `python <script>.py` works when `python` resolves to a native interpreter executable; direct script launch, general multi-GPU, and heterogeneous agents are not qualified. |
| Attachment | Only newly launched targets are supported; attaching to an existing process is absent. |
| KFD events | WDDM has no implemented equivalent for KFD page, queue-eviction, or migration event services. |
| Additional output formats | Perfetto/PFTrace and OTF2 output are absent and are not required by the current FeatherOps workflow. |
| Additional domains | RCCL, rocDecode, rocJPEG, OMPT, Kokkos, and similar optional domains are absent. |

## Installation and build runbook

### Installation and package layout

Windows follows the split package layout used by the ROCm Python environment:

```text
<venv>\Lib\site-packages\_rocm_sdk_devel
<venv>\Lib\site-packages\_rocm_sdk_core
<venv>\Lib\site-packages\rocprofv3
<venv>\Scripts
```

`ROCM_PATH` names `_rocm_sdk_devel`. The package-owned Windows build scripts use `ROCM_PATH` as the default CMake install prefix and use the semantic package build directories below the repository `build` directory:

```text
build\rocprofiler-register
build\aqlprofile
build\rocr-runtime
build\clr-hip
build\rocprofiler-sdk
```

The scripts do not create a package `stage`, milestone directory, candidate prefix, or nested component build. Build state and validation logs remain in the component build directory. An explicit build directory or install prefix is reserved for a deliberate comparison or a separate checkout.

#### Direct replacement

Component builds validate their outputs before installation, then replace only the files owned by that component in `ROCM_PATH`. The replacement helper copies to a temporary sibling, removes the old destination, and moves the new file into place. This both permits ordinary version replacement and breaks package-cache hard links before any new file is installed.

Runtime DLLs that belong to the split core package are copied independently into `_rocm_sdk_core` with the same replacement rule. Windows uses independent files rather than POSIX links. The SDK installation also refreshes the ordinary venv forwarding wrappers in `Scripts`; those wrappers call the launcher below `_rocm_sdk_devel/bin`.

Build-owned `validation` and `provenance` evidence stays below the component build directory. It is never copied into either installed package. System32 is not an installation destination, and the HIP runtime build verifies that an existing System32 HIP DLL remains unchanged.

#### Installed launcher contract

The installed `rocprofv3.cmd` and `rocprofv3-avail.cmd` resolve the Python backend and all Windows DLL directories relative to `_rocm_sdk_devel`. The venv-level wrappers forward to those package launchers, so a normal activated environment supports:

```powershell
rocprofv3 --pmc SQ_WAVES -- target.exe
```

Installed-prefix validation writes its records to the component build validation directory rather than adding marker files to the package. It checks version, help, relocation, availability, ROCTX, ordinary PMC behavior, ROCpd conversion, and the packaged converter/schema assets against the files that were just installed.

### Canonical build and validation operations

The package-owned entry points are authoritative. Activate the target Python environment, set `ROCM_PATH` to its `_rocm_sdk_devel` directory, and use the default paths unless comparing a separate version. After a ROCm pip package reinstall, run registration, ROCr/libhsakmt, HIP/CLR, AQL Profile, and then the SDK; ROCr restores the HSA headers required by AQL Profile, and HIP/CLR publishes the final `amdhip64` DLL and import library used by AQL Profile.

ROCr/libhsakmt and HIP/CLR require the repository-pinned WKMI import library at `shared/amdgpu-windows-interop/wkmi/win/lib/wkmi.lib`. The current pinned object is 448252 bytes with MD5 `d5d5d50aa73f85886029e3dcbce7f03f`. The build scripts verify its size and DVC-pinned MD5 and never download or substitute it automatically. If the file is absent, materialize the pinned object before rerunning the failed component:

```powershell
$env:VIRTUAL_ENV\Scripts\python.exe .\fetch_wkmi_dvc.py
```

#### Registration

```powershell
.\projects\rocprofiler-register\scripts\build_windows.ps1
```

Builds and installs the native PE registration library into `ROCM_PATH`.

#### ROCr/libhsakmt

```powershell
.\projects\rocr-runtime\scripts\build_windows.ps1
```

Validates the pinned WKMI object, builds the registration-enabled runtime, and runs packet publication, WDDM adapter, and non-loading runtime checks before replacing the ROCr runtime files in `ROCM_PATH`. If `shared/amdgpu-windows-interop/wkmi/win/lib/wkmi.lib` is missing, run `fetch_wkmi_dvc.py` from the repository root with the active venv Python and then rerun this step.

#### HIP/CLR runtime

```powershell
.\projects\clr\scripts\build_windows_runtime.ps1
```

Builds the matching HIP/CLR/embedded ROCr runtime with kpack loading enabled, runs the binary and registration checks, and installs the runtime DLL and import library directly into `ROCM_PATH`. Binary validation requires both `rocprofiler-register.dll` and `rocm_kpack.dll`, while continuing to reject a separate `hsa-runtime64.dll` dependency because ROCr remains embedded. Run the AQL Profile entry point separately after the HIP runtime is installed.

#### AQL Profile

```powershell
.\projects\aqlprofile\scripts\build_windows.ps1 -BuildProbe
```

Builds and installs AQL Profile and runs the offline gfx1151 packet check and the loader check. The packet probe runs before installation because it loads the build-tree DLL directly. The loader probe runs after installation because ROCr resolves AQL Profile through the standard installed `hsa-amd-aqlprofile64.dll` name during `hsa_init`; this keeps first rebuilds after a pip package reinstall bootstrap-safe.

#### ROCProfiler SDK

```powershell
.\projects\rocprofiler-sdk\scripts\build_windows.ps1 `
  -BuildAllTargets -RunIntegrationTests
```

The SDK script runs the unit and safe integration gates, installs directly into `ROCM_PATH`, refreshes the split core runtime files and venv wrappers, and runs the installed-prefix checks from the build validation directory.

Use `-SkipInstall` for build-tree validation only, `-ConfigureOnly` for configuration only, and `-Clean` to remove one component's default build folder. Use `-InstallPrefix` and `-BuildDirectory` only when intentionally comparing an installation with the active version. Alternate-prefix publication does not update the active `_rocm_sdk_core` package or venv wrappers.

An explicit alternate `-InstallPrefix` selects its registration, HIP, HSA, AQL Profile, header, import-library, and runtime dependencies from that prefix. A missing dependency fails with its exact expected path; it never falls back to the active venv. Component scripts expose dependency-root overrides for deliberate nonstandard comparisons. Alternate-prefix configuration and publication do not mutate active `_rocm_sdk_devel`, `_rocm_sdk_core`, venv wrappers, or System32.

The complete configured graph can be run with:

```powershell
ctest --test-dir C:/rocm-systems/build/rocprofiler-sdk `
  -C RelWithDebInfo --output-on-failure -j 8
```

## Validation and release qualification

### Windows graph

The maintained Windows SDK graph covers the component, SDK, process, and installed-prefix tests configured in the default build directory. It includes:

- common PMC contract checks;
- CLI and job-object unit/process tests;
- normal, late, and reentrant registration;
- availability, baseline HIP, kernel trace, HIP trace, graph, marker, ROCTX, no-overwrite, and finite HSA barrier execute/validate tests;
- callback, buffered, concurrent, and grouped dispatch counting;
- ordinary `rocprofv3` PMC process tests;
- CPU-only and HIP installed-prefix tests.

The PMC process suite covers CSV, JSON, ROCpd, multipass, filtering, selected regions, composition, statistics, resource metadata, unknown counters, no dispatch, target status, no-replace publication, conversion failure, and fresh processes that prove callback drain before serialization. Installed and build-tree process tests use the maintained `tasklist.exe` and exact-path `Win32_Process` checks after target execution.

The installation checks cover direct replacement, independent split-package files, package-cache hard-link safety, no evidence leakage, System32 non-modification, and successful HIPK kernel loading from the installed gfx1151 PyTorch kpack when that package is present.

### Required validation set

Validate branch changes from `pc_sampling_gfx1151` as a layered gate. Component scripts validate their owned code before publication, and the SDK graph validates the composed installed behavior.

Run the component entry points against the active environment:

```powershell
.\projects\rocprofiler-register\scripts\build_windows.ps1
.\projects\rocr-runtime\scripts\build_windows.ps1
.\projects\clr\scripts\build_windows_runtime.ps1
.\projects\aqlprofile\scripts\build_windows.ps1 -BuildProbe
.\projects\rocprofiler-sdk\scripts\build_windows.ps1 -BuildAllTargets -RunIntegrationTests
```

The component gates must include the tests owned by each package:

- AQL Profile: `aqlprofile.windows.loader` and `aqlprofile.windows.pmc-packet.gfx1151`;
- ROCr/libhsakmt: `hsakmt.windows.packet-publication`, `hsakmt.windows.profiling-adapter`, and `rocr.windows.runtime-binary`;
- CLR/HIP: `clr.windows.runtime-binary`, `clr.windows.registration.normal`, and `clr.windows.registration.late`;
- SDK installed behavior: build-tree tests, active installation, split-package refresh, venv wrappers, and installed-prefix validation.

The complete SDK CTest graph is the normal release gate:

```powershell
ctest --test-dir C:/rocm-systems/build/rocprofiler-sdk `
  -C RelWithDebInfo --output-on-failure -j 8
```

The graph must cover the following named tests or equivalent maintained successors:

- `rocprofiler-sdk.windows.rocprofv3.unit`;
- `rocprofiler-sdk.windows.pmc-contract`;
- `rocprofiler-sdk.windows.aqlprofile-mirror`;
- `rocprofiler-sdk.windows.build-prefix`;
- `rocprofiler-sdk.windows.job-object`;
- `rocprofiler-sdk.windows.rocprofv3.process`;
- `rocprofiler-sdk.windows.rocprofv3.pmc-process`;
- registration tests for normal, late, reentrant, concurrent, multi-client, duplicate-finalize, and finalization-failure behavior;
- dispatch-counting tests for callback, buffered, concurrent, and grouped modes;
- execute/validate pairs for availability, baseline HIP, kernel trace, HIP trace, HIP graph, HIP marker, ROCTX trace, no-overwrite, and HSA barrier;
- installed-prefix CPU-only and HIP tests.

Installed-package validation is required because the supported user path is the wrapper and active package, not a private build-tree launcher. It must check `rocprofv3 --version`, `rocprofv3 --help`, `rocprofv3-avail list`, `list --pmc`, `info --pmc`, representative `pmc-check` commands, the 40 plain-C SDK exports, absence of installed `validation` and `provenance` directories, byte-identical split devel/core DLLs, hard-link independence, wrapper relocation, System32 HIP non-modification, packaged ROCpd converter/schema assets, and real CSV/JSON/ROCpd output from `rocprofv3 --pmc COUNTER1 COUNTER2 -- target.exe`. It also direct-loads the installed availability DLL in a fresh Python process, rejects the obsolete nested location, queries ROCpd integrity and foreign keys, and requires `rocm-sdk test` to load every devel/core shared library successfully.

A live PMC reliability gate should remain an opt-in release stress label rather than part of every local build. It runs repeated installed-wrapper processes, reserves fresh outputs, requires every artifact for the selected CSV, JSON, and ROCpd formats, checks dispatch IDs and positive values, trusts the target exit status, and verifies cleanup with both `tasklist.exe` and exact-path `Win32_Process` queries. It must not assert exact `SQ_WAVES` totals.

### Test reuse and consistency

New Windows rocprofv3 tests should reuse the maintained CTest and pytest infrastructure instead of adding standalone scripts. All pytest-based CTest entries must continue to run through `tests/windows/common/run_pytest.py`, use a build-owned temporary root, and let the pytest process status decide pass or fail.

Process-oriented tests should share the same helpers for environment scrubbing, job-object execution, target cleanup, private-result parsing, expected-output checks, CSV/JSON/ROCpd parsing, dispatch identity, and positive counter assertions. The canonical patterns are in `tests/windows/rocprofv3/test_windows_pmc_process_cli.py`, `tests/windows/validate_windows_install.py`, `tests/windows/rocprofv3/windows_integration.py`, and `tests/windows/common/process_cleanup.py`; future work should keep converging that duplicated logic into common modules under `tests/windows/common`.

Prefer declarative case matrices over bespoke one-off runners for ordinary PMC, filtering, multipass, selected regions, composed tracing plus PMC, unknown counters, no dispatch, target failure, publication conflict, existing output, and repeated fresh-process cases. Use the existing execute/validate fixture pattern for integration cases that produce files and a separate validator for assertions.

The gfx1151 availability and metadata consistency point is `tests/pmc-parity/test_pmc_contract.py` with `tests/pmc-parity/gfx1151_linux_contract.json`. Availability parity changes should update that contract intentionally and then qualify the same source on Windows and Linux.

### Linux isolation and semantic oracle

Linux host `x2` is a semantic oracle, not a Windows runtime dependency. The oracle is maintained evidence rather than an immutable record of every current Linux behavior. When a focused test proves an existing Linux behavior is internally inconsistent, such as split dispatch selection or raw rather than 512-byte-normalized ROCpd LDS fields, fix Linux and update the machine-readable contract in the same change. The contracts are:

```text
projects/rocprofiler-sdk/tests/pmc-parity/gfx1151_linux_contract.json
projects/rocprofiler-sdk/tests/dispatch-analysis/gfx1151_linux_oracle.json
projects/rocprofiler-sdk/tests/dispatch-analysis/behavior_cases.json
```

They record the 442-entry catalog, dimensions, expressions, compatible and rejected profiles, CLI structure, multipass output, filtering, composition, failure behavior, dispatch-analysis shapes, statistics, resource metadata, and ROCpd view semantics. `pmc_contract.py` canonicalizes only intentionally variable paths, provenance, identities, timestamps, numeric values, and branding.

The implementation files qualified on `x2` are byte-for-byte identical to the release source. The native build, local contract checks, compatible/rejected profile observations, repeated PMC processes, and `LD_DEBUG=libs` library-resolution check are the Linux qualification gate.

Linux repeated PMC validation requires stable record shape, dispatch identity, and positive hardware values rather than exact `SQ_WAVES`, which varies naturally between runs.

### Release invariants

A releasable candidate satisfies all of the following:

- ordinary installed `rocprofv3 --pmc ... -- target.exe` works without private activation controls;
- discovery and configuration use standard SDK APIs and the common catalog;
- records use standard SDK identities and CSV, JSON, statistics, and ROCpd schemas;
- composed traces, counters, statistics, resources, and ROCpd rows agree by authoritative dispatch identity;
- concurrent queues and common profile groups work without a fallback engine;
- invalid capabilities or profiles fail before unsafe submission;
- output is reserved and published without replacement;
- target status and no-dispatch behavior match the common contract;
- the active devel/core/Python/wrapper installation is complete and relocatable;
- build-owned evidence is outside the installed packages;
- package caches and System32 remain unchanged;
- Windows and same-source Linux validation pass.

### Retained qualification evidence

Component build directories retain the validation logs for the corresponding source and installation. The maintained evidence locations are:

```text
build\rocprofiler-register\validation
build\aqlprofile\validation
build\rocr-runtime\validation
build\clr-hip\validation
build\rocprofiler-sdk\validation
```

The Linux semantic contracts remain in `tests/pmc-parity/gfx1151_linux_contract.json`, `tests/dispatch-analysis/gfx1151_linux_oracle.json`, and `tests/dispatch-analysis/behavior_cases.json` below `projects/rocprofiler-sdk`. Release review checks active package contents, split-package coherence, package-cache safety, System32 non-modification, direct installed CSV/JSON/ROCpd output, and same-source Linux behavior.

## Implementation details and evidence

The post-enablement convergence pass is implemented in the final source. It does not introduce a new hardware-enablement phase: it preserves qualified gfx1151 behavior, the public 64-byte packet ABI, the existing producer-process trust model, the Windows-minimal service boundary, and direct active-package installation.

### Registration and lifecycle

Native Windows registration now uses a mutex, condition variable, explicit lifecycle state, and initialization-owner tracking. Concurrent HSA, HIP, and ROCTX registration waits until SDK initialization has completed or failed rather than observing a transient early return. Tool loading, `rocprofiler_configure`, client initialization, and producer API-table callbacks remain outside the `rocprofiler-register` mutex.

Reentrant producer-table registration by the initialization owner copies table pointers into deferred storage. The deferred tables are drained after all clients initialize, so reentrancy does not deadlock and does not expose partial state to waiting threads.

Every valid `ROCP_TOOL_LIBRARIES` entry is retained as a separate client record with its own immutable client ID, tool data, initialization state, and finalizer. Priority greater than zero does not reject a valid Windows client. Initialization/configuration exceptions become explicit SDK initialization failures. Finalization is serialized, stops client contexts while SDK workers and queue infrastructure remain live, deactivates client contexts, and invokes each initialized client finalizer exactly once. Duplicate explicit finalize requests are idempotent. The Windows tool no longer duplicates context stopping and releases its state after serialization.

Maintained registration tests prove:

- normal and late API-table propagation;
- reentrant propagation during client initialization;
- concurrent waiters cannot observe partial initialization;
- two independently configured clients initialize and finalize once each;
- repeated finalization does not invoke either finalizer again.

### Shared counter and output behavior

Dependency-light common helpers now own `ROCPROF_COUNTERS`/`ROCPROF_COUNTER_GROUPS` parsing, group validation, one-based iteration-range parsing with overflow protection, and the standard counter CSV columns. Linux and Windows use the same CSV quoting implementation, including embedded-quote escaping. Windows keeps native queue adaptation, PE configuration, trace ingestion, and process metadata in its platform implementation.

Counter JSON serialization copies and sorts records by dispatch ID before writing. CSV and JSON therefore preserve deterministic dispatch order under concurrent and reversed completion. The Windows implementation retains formatted kernel-name filtering, selected-region marker control, common output schemas, and Windows LLP64 spelling without pulling Linux-only output or runtime services into the minimal build.

### SDK API boundary

`ROCPROFILER_BUILD_WINDOWS_MINIMAL` remains the sole Windows service-boundary option. Dispatch counting is mandatory in that build; the former `ROCPROFILER_BUILD_WINDOWS_DISPATCH_COUNTING` switch and all unreachable disabled branches are removed.

The installed public headers define `ROCPROFILER_SDK_WINDOWS_MINIMAL_API` as `1` for native Windows and `0` elsewhere. Its API documentation identifies the supported dependency-light subset and the Linux-only KFD, PC-sampling, ATT, SPM, attachment, and optional output-service boundary. Consumers can select the native subset at compile time instead of inferring it from a missing PE symbol. The native DLL audit found 40 plain-C `rocprofiler_*` exports covering lifecycle and status queries, agents and counters, contexts and buffers, callback and buffered dispatch counting, the bounded callback-tracing path, external correlation, thread callbacks, and producer registration. APIs beyond that declared cross-platform source interface remain explicitly outside `ROCPROFILER_SDK_WINDOWS_MINIMAL_API` rather than being represented by a second runtime feature switch.

### Availability and failure propagation

Windows availability uses checked initialization and explicit status transport. Agent discovery, counter iteration, metadata lookup, and dependency errors cannot silently produce a successful empty or partial catalog. Catalog dimensions remain topology-only: availability does not construct collection packets, validate every hardware descriptor, or emit profile-planning diagnostics.

The lightweight availability DLL preserves deterministic counter, dimension, and agent sorting. Valid `list`, `list --pmc`, `info --pmc`, and representative `pmc-check` output matches same-source Linux after CRLF/LF normalization. Native agent output differs only in the documented `gpu_id`, `product_name`, and `model_name` fields.

A private result file now carries SDK and tool setup, initialization, profiling, finalization, serialization, and publication status to the Windows launcher. The launcher distinguishes those failures from unknown-counter and no-dispatch success. Unknown counters still warn with `Unable to find counter`, emit no counter files, and preserve target status.

All result and output publication operations are checked. Once any client publishes a private failure status, later successful client finalizers cannot downgrade it. Effective outputs are reserved before target side effects. A target-created conflict is preserved, and only a profiler-created partial output is removed. Failed private status publication emits a deterministic SDK/tool diagnostic rather than being accepted as target success.

### Comparison prefixes and installation ownership

All package scripts normalize `InstallPrefix` before dependency selection. Ordinary builds install directly into the active `ROCM_PATH`; a deliberate alternate prefix resolves matching registration, HIP, HSA, AQL Profile, headers, libraries, and runtime DLLs from that prefix. A missing exact dependency fails without falling back to the active venv.

Replacement removes each component-owned destination before copying, breaking package-cache hard links without deleting foreign namespaces. Runtime DLL synchronization into `_rocm_sdk_core` and wrapper updates occur only for active split-package publication. CLR publishes only CLR/HIP-owned artifacts and never executes its generated prefix-wide install graph. Validation and provenance stay in component build directories, and no script modifies `C:/Windows/System32/amdhip64_7.dll`.

The maintained prefix-isolation test hashes the active SDK, requests alternate-prefix configuration with an incomplete runtime, verifies the exact failure, and proves both the alternate prefix and active package remain unchanged.

### Maintained tests and cleanup

Every Windows pytest CTest entry runs through `tests/windows/common/run_pytest.py` and uses a build-owned temporary root. CTest accepts or rejects the pytest process exit status directly; no `PASS_REGULAR_EXPRESSION` can mask a nonzero result.

Installed and build-tree process tests call shared cleanup checks after every target execution. They query both `tasklist.exe` by exact image name and `Win32_Process` by exact executable path. Publication-conflict, availability-initialization, private-result, build-prefix, concurrent-registration, multi-client, and mirrored AQL Profile metadata/frame/bounds coverage are maintained tests rather than one-off evidence.

Dead retained scan fields, unused attach/detach lookups, obsolete constructor activation, duplicated client state, secondary dispatch switches, tautological guards, and qualification-phase names in production paths were removed. Standalone and embedded AQL Profile metadata remain checked by CPU-only mirror tests.

### Windows completion evidence

The current Windows source and active installation passed:

- the maintained `build_windows.ps1 -RunIntegrationTests` flow, all 20 execute/validate integrations, build-prefix validation, and both installed-prefix gates;
- the complete configured SDK graph, 42 of 42 tests, including registration, descriptor decoding, output readers, callback/buffered/concurrent/grouped dispatch counting, process tests, and installed CPU/GPU qualification;
- standalone AQL Profile loader and gfx1151 packet probes plus ROCr packet-publication, profiling-adapter, and runtime-binary checks;
- kpack-enabled CLR binary/registration checks, the installed PyTorch HIPK probe, FP16 PyTorch work, and all 28 custom HIP FP16 kernel configurations;
- the 442-entry gfx1151 catalog matched the expected metadata contract, with positive installed collection for the qualified representative counters;
- installed six-dispatch composition with six timed kernels, 18 counters, three statistics rows, three symbols, and a ROCpd 3.0.3 database with clean integrity and foreign keys;
- `rocm-sdk test` with 27 tests, zero failures or errors, and the expected Linux-only LLVM-symlink skip;
- no remaining fixture process, no installed `validation` or `provenance`, 40 plain-C SDK exports, coherent split-package copies, and an unchanged System32 HIP runtime.

The exact qualified source was also built and installed from an empty prefix on `x2`. The shared behavior and descriptor tests passed; repeated dispatch-analysis runs retained stable six-kernel, 18-counter, three-statistics, and three-symbol shapes with positive values; native ROCpd 3.0.3 produced the matching six dispatches, 18 PMC events, and three summaries; and `LD_DEBUG=libs` resolved the SDK and tool from the qualification prefix.
