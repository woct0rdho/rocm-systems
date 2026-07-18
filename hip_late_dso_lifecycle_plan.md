# HIP Late-Loaded DSO Lifecycle Design

## Status

This design is implemented and validated on the TheRock/gfx1151 stack. It makes compiler-generated HIP fat-binary registration and teardown safe when a HIP shared object is loaded after HIP has already initialized and submitted GPU work.

The runtime changes are intentionally confined to static code-object registration, lazy materialization, and cleanup. They do not require application workarounds such as `LD_PRELOAD`, `RTLD_NODELETE`, import reordering, package pinning, or bypassing normal process finalization.

## Problem Statement

A HIP DSO contains compiler-generated constructors and destructors that call the HIP runtime entry points for static fat-binary registration:
- `__hipRegisterFatBinary()` or the kpack equivalent registers the module.
- `__hipRegisterFunction()`, global registration, and managed-variable registration associate host symbols with that module.
- `__hipUnregisterFatBinary()` runs from a compiler-generated DSO destructor during `dlclose()` or process finalization.

This lifecycle is straightforward when every HIP DSO is loaded before runtime initialization. It becomes unsafe when an application performs the following sequence:
- Initialize HIP and create device, stream, allocator, and command state.
- Load another HIP DSO with `dlopen()` or through a Python extension import.
- Optionally use a kernel or device symbol from that DSO.
- Unload the DSO explicitly or let the dynamic loader finalize it at process exit.

Before this change, late registration could eagerly digest the fat binary from inside the DSO constructor. Unregistration then performed process-global device synchronization and destroyed registry objects while runtime and host-language teardown were already in progress. Python/PyTorch applications exposed this as allocator corruption or a libc segfault during otherwise normal process finalization.

The failure was sensitive to import order and heap layout, but import order was not the root cause. The same native modules could be present in both passing and failing processes. The important difference was when compiler-generated finalizers ran relative to Python, PyTorch, HIP, HSA completion handling, and allocator teardown.

## Design Goals

The implementation provides these guarantees:
- Loading a HIP DSO is metadata-only, even after HIP initialization.
- A module is materialized only when an application actually resolves one of its functions or variables.
- Compiler-generated unregister callbacks are null-tolerant, idempotent, and non-aborting.
- Registry mutation is completed under the static-code-object lock, while object destruction occurs after releasing that lock.
- Per-module unregister and global runtime cleanup use the same ownership-transfer model.
- Compiler-generated finalizers never invoke general stream or device synchronization.
- Explicit `dlclose()`, repeated load/unload, concurrent load/unload, arbitrary DSO finalizer order, device globals, and managed variables remain supported.
- `HIP_ENABLE_DEFERRED_LOADING=0` does not force code-object creation from a DSO constructor.
- HIPF and TheRock kpack registration retain the same lazy lifecycle.

## Non-Goals

This change does not redefine all possible unsafe `dlclose()` behavior. In particular, an application must not unload a DSO while an outstanding kernel still directly accesses storage whose ownership belongs to that DSO, such as a module-owned managed variable. The pending-work test uses a kernel-only fixture, where command and kernel objects retain their own references after registry ownership is released.

The design also does not change normal HIP API synchronization semantics, allocator teardown, stream implementation, or HSA async-handler behavior. It avoids invoking those general mechanisms from a context where the host runtime may already be partially finalized.

## Lifecycle Model

A registered static HIP module has three conceptual states.

| State | Registry entry | `FatBinaryInfo` | Meaning |
|---|---|---|---|
| Registered | Present | `nullptr` | Compiler metadata is known, but no code object has been digested. |
| Materialized | Present | Non-null | A function, global, or managed symbol was requested and the code object was loaded. |
| Detached | Absent | No registry ownership | Per-module unregister or global cleanup transferred ownership out of the registry. |

The module handle returned to compiler-generated registration code is a stable `FatBinaryInfo**` pointing into the module registry. `try_emplace()` preserves the existing entry on duplicate registration rather than replacing a live handle.

The important state transition is:

```text
registered metadata
        |
        | first checked symbol lookup
        v
materialized code object
        |
        | per-module unregister or global cleanup
        v
detached cleanup batch
        |
        | destruction outside the registry lock
        v
released
```

An unused late-loaded DSO remains in the registered state until it is detached. It never creates COMGR programs or device code merely because its constructor ran after HIP initialization.

## Registration Design

### Metadata-only fat-binary registration

`StatCO::AddFatBinary()` and `StatCO::AddKpackBinary()` only create or find the registry entry and record the host-module key. They no longer call `DigestFatBinary()` based on `PlatformState::IsInitialized()`.

This rule is unconditional because a DSO constructor is not a safe place to materialize the module:
- Function, global, and managed-variable registration callbacks may not all have run yet.
- The dynamic loader is still executing the DSO's initialization sequence.
- HIP may already have active streams, allocators, and completion callbacks from unrelated work.
- Eager materialization creates teardown ownership for a module the application may never use.

The same rule applies to HIPF and kpack wrappers. The wrapper address remains the lookup key so the later lazy path can rediscover the original image and kpack metadata.

### Metadata-only function registration

`__hipRegisterFunction()` records the host function and its module association but does not initialize HIP or resolve the device function. This remains true when `HIP_ENABLE_DEFERRED_LOADING=0`.

That environment setting can still control other runtime behavior, but it must not make a compiler-generated DSO constructor digest a partially registered module. Registration callbacks define metadata; application-visible symbol use triggers materialization.

### Checked lazy materialization

`StatCO::EnsureFatBinaryLoaded()` centralizes lazy code-object creation. Function lookup, function-attribute lookup, device-global lookup, and managed-symbol initialization all use this helper.

The helper:
- Rejects a null module handle.
- Finds the handle in `module_to_hostModule_` without inserting anything.
- Returns an error if the module has already been detached or was never registered.
- Calls `DigestFatBinary()` only when the registered `FatBinaryInfo*` is still null.

Checked lookup is important during concurrent or late teardown. The old `operator[]` paths could recreate a mapping after unregister, turning a stale symbol lookup into new registry state. The new paths fail instead of resurrecting detached ownership.

## Managed-Variable Handling

Late managed-variable registration is protected by `StatCO::sclock_`, like the rest of the static registry.

When a new managed variable arrives, every per-device `managedVarsDevicePtrInitalized_` flag is invalidated. A device that initialized managed symbols before the DSO was loaded must revisit the registry and initialize the new module's symbols on the next managed-symbol pass.

Cleanup gathers both host-side managed allocations and device-specific allocations into the detached cleanup operation. A rich DSO fixture verifies lazy loading and teardown of kernels, device globals, and managed variables together.

## Unregistration Design

### Idempotent compiler finalizer

`__hipUnregisterFatBinary()` treats a null handle as a no-op and delegates to `StatCO::RemoveFatBinary()` without `guarantee()` or process abort behavior.

A compiler-generated destructor can legitimately observe a module that was already removed by:
- an earlier explicit `dlclose()` path,
- global HIP teardown,
- duplicate or repeated finalizer execution, or
- partial registration failure.

Unknown and already-removed handles are therefore successful no-ops. Finalizer bookkeeping must not turn normal process shutdown into an abort.

### Two-phase detach and destroy

`RemoveFatBinary()` uses a two-phase ownership transfer:
- Acquire `sclock_`.
- Find the module and detach all associated functions, variables, managed variables, and `FatBinaryInfo` objects from registry containers.
- Store the detached pointers in `FatBinaryCleanup`.
- Release `sclock_`.
- Destroy the detached objects through `DestroyFatBinaryCleanup()`.

This split establishes a clear ownership boundary. Once detachment finishes, no registry lookup can find or recreate the module. Destructors, memory release, and program destruction then run without holding the global static-code-object lock.

Destroying outside the lock avoids lock re-entry and lock-order problems with allocators, devices, HSA objects, and code-object destructors. It also reduces the critical section to registry mutation rather than potentially expensive runtime cleanup.

### Shared global cleanup

`RemoveAllFatBinaries()` uses the same `DetachFatBinaryLocked()` and `DestroyFatBinaryCleanup()` machinery as per-module unregister.

Global cleanup first collects every registered module handle, detaches each one, and then defensively collects any orphaned function, variable, managed-variable, or fat-binary entries left by a partial registration failure. Registry maps and managed initialization state are cleared before destruction begins.

Using one cleanup model gives per-module and global teardown the same ordering and idempotency properties. A later DSO destructor becomes a harmless no-op after global cleanup has already detached its module.

## Why Finalizer-Time Synchronization Was Removed

The previous `__hipUnregisterFatBinary()` used a process-global `std::call_once` to synchronize every device before removing the first static module. This was unsafe for two independent reasons:
- The first finalizer was often an unused module with no materialized code object, so synchronization was unrelated to that module's ownership.
- At process exit, a compiler-generated DSO destructor may run after Python, PyTorch, allocator, queue, or completion-handler teardown has already begun. A general HIP synchronization call is not valid merely because the DSO still has a registration callback to run.

Restricting synchronization to materialized modules improved the initial cases but did not solve the lifecycle. A late import-order reproducer still failed when loaded modules synchronized from compiler finalizers.

Investigation separated several possible causes:
- Skipping allocator reclamation while retaining stream waits was insufficient.
- Waiting for tracked HSA async handlers after stream completion was insufficient.
- Deleting detached static code-object ownership without finalizer-time stream synchronization was sufficient.
- Explicit application synchronization before host-runtime teardown also avoided the failure, confirming that synchronization is safe when initiated by the application while the runtime is intact, not from an arbitrary loader finalizer.

The final rule is therefore simple: static fat-binary unregister detaches and releases registry ownership but does not act as a general HIP synchronization point.

Loaded kernels and programs are reference-counted by commands that use them. Releasing static registry ownership does not invalidate an in-flight kernel command that already holds its own references. This is validated by launching a plugin kernel, unloading the kernel-only DSO before application synchronization, and then successfully synchronizing through a still-loaded host module.

## Concurrency and Ownership Invariants

The implementation relies on these invariants:
- `sclock_` protects the static module registry, symbol maps, managed-variable registry, and lazy materialization decision.
- A module is either owned by the registry or by a local `FatBinaryCleanup` batch, never both.
- Detachment removes lookup visibility before any detached object is destroyed.
- Lazy lookup uses `find()` and cannot recreate a detached module.
- Duplicate registration preserves the original stable module-handle address.
- Per-module unregister and global cleanup are mutually idempotent.
- Compiler-generated finalizers do not synchronize devices, reclaim unrelated pools, or abort the process.
- Destruction that can interact with other runtime subsystems occurs after releasing `sclock_`.

The integrated regression includes four threads repeatedly loading, using, and unloading the same plugin to exercise these rules under concurrent registration and teardown.

## Test Architecture

### Integrated hip-tests fixture

The upstream-tree regression under `projects/hip-tests/catch/unit/dynamicLoading` builds two independent compiler-generated HIP DSOs from the same source with different identifiers. Each DSO contains:
- a kernel,
- a device global,
- a managed variable,
- an exported launch function, and
- an exported expected-result function.

A standalone child executable controls the dynamic-loader lifecycle. Catch2 launches each mode in an isolated child process and checks the real process exit status, including failures caused by signals during finalization.

The matrix covers:
- load before HIP initialization,
- unused late load followed by process exit,
- used late load followed by process exit,
- explicit unload,
- 100 repeated load/use/unload cycles,
- four-thread parallel load/use/unload cycles,
- two-DSO A/B and B/A load/finalizer order,
- two-DSO A/B and B/A explicit unload order, and
- both `HIP_ENABLE_DEFERRED_LOADING=0` and `1`.

The integrated Catch2 test passes all 72 assertions across both deferred-loading settings.

### Package-independent local fixture

The top-level `test/hip_late_dso` fixture intentionally uses a smaller kernel-only DSO. It reproduces late loading after existing GPU work without device-global or managed-variable constructor activity, making it useful for isolating registration and finalization behavior.

Its host covers:
- load before initialization,
- unused and used process-exit paths,
- explicit unload,
- unload with plugin work still pending,
- and repeated load/use/unload cycles.

`test_hip_late_dso.sh` accepts one case per invocation, runs it against the installed runtime, applies `HSA_DISABLE_XDNA=1` for the known unrelated XDNA discovery failure on the validation machine, records output, and performs process/kernel-log follow-up after nonzero exits. Invoking the script without a case prints usage and exits with status 2; the approved matrix invokes it once for each of the 12 cases.

### Python and package regressions

The Python fixture initializes PyTorch GPU state before loading the test DSO through `ctypes`. It covers both unused and used late modules under the host-language finalizer order that originally exposed the defect.

Additional package-level cases exercise realistic native-extension and import-order behavior:
- PyTorch with an unused late HIP DSO,
- PyTorch with a used late HIP DSO,
- bitsandbytes after GPU initialization,
- bare Unsloth import,
- `torch`, `peft`, then Unsloth,
- and the full `torch`, `transformers`, `peft`, `trl`, `unsloth`, `unsloth_zoo` sequence.

The integrated Catch2 matrix exits normally for both `HIP_ENABLE_DEFERRED_LOADING=0` and `1`. All 12 one-case invocations of `test_hip_late_dso.sh` also exit normally against the installed runtime, but intentionally do not sweep that environment variable. No case requires preload, nodelete, import reordering as a workaround, package pinning, or `os._exit()`.

### Existing module coverage

Focused existing HIP tests continue to cover driver-module loading and unloading, static function lookup, managed variables, device globals, and positive module launches. These tests complement rather than replace the late-DSO fixture: ordinary `hipModuleLoad()` tests do not execute compiler-generated `__hipRegisterFatBinary()` constructors from a late `dlopen()` DSO.

## Validation Summary

The approved rerun used the rebuilt installed runtime reporting version `7.16.26326-9095425e29`. The finalized runtime passed the following coverage on gfx1151:
- all 12 integrated child modes with both deferred-loading values,
- the Catch2 late-DSO regression with 72 assertions,
- all 12 standalone installed-runtime cases from `test_hip_late_dso.sh`,
- 100 repeated explicit unload cycles,
- four-thread parallel load/use/unload cycles,
- pending-work explicit unload for the kernel-only fixture,
- device-global and managed-variable DSO coverage,
- focused existing module load, unload, lookup, global, managed, and launch tests,
- repeated PyTorch unused/used late-DSO cases,
- bitsandbytes and Unsloth package cases,
- minimal and full late-import-order cases,
- and current installed-runtime verification through `test_hip_late_dso.sh`.

CTest discovery in this build tree currently stops before filtering because the generated include for an empty disabled `dynamicLoading` registration is missing. The integrated result above therefore comes from invoking the `hipLateDsoLifecycle` Catch2 executable directly with the `Unit_hipLateDsoLifecycle_ProcessFinalization` filter; that command passed all 72 assertions. This is a CTest registration/discovery issue, not a late-DSO runtime failure.

The final design was validated without `LD_PRELOAD`, `RTLD_NODELETE`, package-version changes, import-order workarounds, or process-finalization bypass.

## Implementation Boundaries

### Runtime implementation

The lifecycle behavior is implemented in:
- `projects/clr/hipamd/src/hip_code_object.cpp`
  - metadata-only HIPF and kpack registration,
  - checked lazy materialization,
  - managed-variable invalidation,
  - two-phase per-module cleanup,
  - shared global cleanup,
  - and destruction without finalizer-time device synchronization;
- `projects/clr/hipamd/src/hip_code_object.hpp`
  - the detached cleanup-batch type and helper declarations;
- `projects/clr/hipamd/src/hip_platform.cpp`
  - metadata-only function registration and null-tolerant, non-aborting unregister callbacks.

Unrelated cluster-size or occupancy changes in `hip_platform.cpp` are outside this design and must not be included in an upstream lifecycle patch.

### Integrated regression

The focused upstream-tree regression is contained in:
- `projects/hip-tests/catch/unit/dynamicLoading/CMakeLists.txt`
- `projects/hip-tests/catch/unit/dynamicLoading/hipLateDsoLifecycle.cc`
- `projects/hip-tests/catch/unit/dynamicLoading/hipLateDsoLifecycle_exe.cc`
- `projects/hip-tests/catch/unit/dynamicLoading/hipLateDsoPlugin.cc`

### Local reproducibility tools

The installed-runtime build and validation workflow is contained in:
- `build_hip_runtime.sh`
- `build_hip_late_dso_test.sh`
- `test_hip_late_dso.sh`
- `test/hip_late_dso/late_dso_plugin.cpp`
- `test/hip_late_dso/late_dso_host.cpp`
- `test/hip_late_dso/torch_late_dso.py`
- `sync_rocm_sdk_links.py`

The small local plugin and the richer integrated plugin serve different purposes and should both be retained: the former isolates the minimal late-load/finalizer interaction, while the latter covers device globals, managed variables, multiple DSOs, and concurrency.

## Rebase Guidance

The runtime patch should be carried as one lifecycle change across future CLR updates. Its essential design is:
- registration callbacks remain metadata-only;
- application symbol use performs checked lazy materialization;
- unregister transfers registry ownership in two phases;
- per-module and global cleanup share the same detach/destroy model; and
- compiler-generated finalizers never perform general device synchronization.

The integrated test commit is independently optional if its CMake integration conflicts during a rebase, but the runtime invariants should not be weakened. The top-level fixture can validate the runtime while test integration is being adapted.

Because this document describes the final design rather than the investigation sequence, historical experiments that synchronized only loaded modules, separated allocator reclamation, or waited for HSA handlers are intentionally recorded only as rejected alternatives. They are not part of the implementation to preserve.
