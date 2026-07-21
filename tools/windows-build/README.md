# Windows package build entry points

The maintained Windows build orchestration is package-owned:

- `projects/rocprofiler-register/scripts/build_windows.ps1`
- `projects/aqlprofile/scripts/build_windows.ps1`
- `projects/rocr-runtime/scripts/build_windows.ps1`
- `projects/clr/scripts/build_windows_runtime.ps1`
- `projects/rocprofiler-sdk/scripts/build_windows.ps1`

Run these scripts from any working directory after activating the target Python environment and setting `ROCM_PATH` to its `_rocm_sdk_devel` directory. Their default build directories are the semantic package directories below the repository `build` directory:

```text
build/rocprofiler-register
build/aqlprofile
build/rocr-runtime
build/clr-hip
build/rocprofiler-sdk
```

There is no package-level `stage`, milestone, or nested candidate directory. CMake configure/build state and validation logs stay in the relevant package build directory. Installation writes directly to `ROCM_PATH` and replaces the package's current files. The shared PowerShell helper copies to a temporary entry, removes the destination, and completes replacement by rename. This prevents partial copies and ensures that a package-cache hard link is never modified in place.

The expected order for a coherent Windows ROCProfiler installation is:

1. Build and install `rocprofiler-register`.
2. Build and install the ROCr runtime.
3. Build and install the HIP/CLR runtime with
   `build_windows_runtime.ps1`.
4. Build and install AQL Profile.
5. Build and install the ROCProfiler SDK with `-RunIntegrationTests`.

The registration, HIP/CLR, AQL Profile, and SDK scripts synchronize the applicable runtime DLLs into the split `_rocm_sdk_core` package. The SDK script also refreshes the ordinary venv forwarding wrappers in `Scripts`. Build-owned validation and provenance data are not copied into an installed package, and System32 is never a destination.

The standalone ROCr and AQL Profile entry points remain available for component validation. The repository-root `build_*_windows.ps1` files are compatibility forwarders; new automation should invoke the package-owned paths above.

An explicit `-BuildDirectory` or `-InstallPrefix` is for a deliberate comparison or a separate checkout only. To construct a coherent comparison prefix, pass the same `-InstallPrefix` to each required package script in dependency order. For an alternate prefix, dependency defaults resolve to that prefix and fail if a required registration, HSA, HIP, AQL Profile, or runtime artifact has not been installed there. Explicit dependency-root parameters remain available when deliberately mixing toolchain inputs.

Split-package synchronization and venv wrapper replacement occur only when the install prefix is the active `_rocm_sdk_devel` directory. Normal development should rely on the defaults: `build/<package>` for build state and `$env:ROCM_PATH` for installation.
