param(
    [string]$VenvPath = $env:VIRTUAL_ENV,
    [string]$BuildDirectory = "",
    [string]$InstallPrefix = "",
    [string]$RuntimeRoot = "",
    [string]$RocprofilerRegisterRoot = "",
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$BuildType = "RelWithDebInfo",
    [switch]$Clean,
    [switch]$ConfigureOnly,
    [switch]$SkipInstall,
    [switch]$BuildAllTargets,
    [switch]$RunIntegrationTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../../.."))
. (Join-Path $repositoryRoot "tools/windows-build/common.ps1")

$sourceDirectory = Join-Path $repositoryRoot "projects/rocprofiler-sdk"
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repositoryRoot "build/rocprofiler-sdk"
}
$layout = Get-RocmVenvLayout -VenvPath $VenvPath
if ([string]::IsNullOrWhiteSpace($InstallPrefix)) {
    $InstallPrefix = $env:ROCM_PATH
}
if ([string]::IsNullOrWhiteSpace($InstallPrefix)) {
    throw "ROCM_PATH must name the active development package or pass -InstallPrefix."
}
$buildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$InstallPrefix = [IO.Path]::GetFullPath($InstallPrefix)
$publishActiveSplit = Test-SameWindowsPath -Left $InstallPrefix -Right $layout.DevelRoot
$defaultDependencyRoot = Get-WindowsDependencyRoot -InstallPrefix $InstallPrefix `
    -ActiveDevelRoot $layout.DevelRoot
if ([string]::IsNullOrWhiteSpace($RuntimeRoot)) {
    $RuntimeRoot = $defaultDependencyRoot
}
if ([string]::IsNullOrWhiteSpace($RocprofilerRegisterRoot)) {
    $RocprofilerRegisterRoot = $defaultDependencyRoot
}
$RuntimeRoot = [IO.Path]::GetFullPath($RuntimeRoot)
$RocprofilerRegisterRoot = [IO.Path]::GetFullPath($RocprofilerRegisterRoot)

if ($InstallPrefix -eq [IO.Path]::GetPathRoot($InstallPrefix)) {
    throw "InstallPrefix must not name a filesystem root: $InstallPrefix"
}
foreach ($requiredRuntimePath in @(
    (Join-Path $RuntimeRoot "bin/amdhip64_7.dll"),
    (Join-Path $RuntimeRoot "bin/amd_comgr.dll"),
    (Join-Path $RuntimeRoot "bin/hsa-amd-aqlprofile64.dll"),
    (Join-Path $RuntimeRoot "bin/hsa-runtime64.dll"),
    (Join-Path $RuntimeRoot "lib/amdhip64.lib"),
    (Join-Path $RuntimeRoot "include/hsa/hsa.h")
)) {
    if (-not (Test-Path $requiredRuntimePath -PathType Leaf)) {
        throw "The Windows runtime installation is incomplete: $requiredRuntimePath"
    }
}
foreach ($requiredRegistrationPath in @(
    "bin/rocprofiler-register.dll",
    "lib/rocprofiler-register.lib",
    "lib/cmake/rocprofiler-register/rocprofiler-register-config.cmake"
)) {
    if (-not (Test-Path (Join-Path $RocprofilerRegisterRoot $requiredRegistrationPath) -PathType Leaf)) {
        throw "The rocprofiler-register installation is incomplete: $RocprofilerRegisterRoot"
    }
}

Import-VisualStudioBuildEnvironment
foreach ($tool in @("cmake.exe", "ninja.exe", "ctest.exe")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool is required and was not found on PATH."
    }
}

$optionDefinition = Select-String -Path (Join-Path $sourceDirectory "cmake/rocprofiler_options.cmake") `
    -Pattern "ROCPROFILER_BUILD_WINDOWS_MINIMAL" -Quiet
if (-not $optionDefinition) {
    throw "The native Windows minimal build option is not defined in the source tree."
}

if ($Clean -and (Test-Path $buildDirectory)) {
    Write-Host "Removing $buildDirectory"
    Remove-Item -Recurse -Force $buildDirectory
}
New-Item -ItemType Directory -Force -Path $buildDirectory | Out-Null
$validationDirectory = Join-Path $buildDirectory "validation"
New-Item -ItemType Directory -Force -Path $validationDirectory | Out-Null

$pythonVersion = (& $layout.Python -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')" | Select-Object -First 1).Trim()
$prefixPath = if ($publishActiveSplit) {
    "$RocprofilerRegisterRoot;$RuntimeRoot;$($layout.CoreRoot)"
} else {
    "$RocprofilerRegisterRoot;$RuntimeRoot"
}
$integrationTestsValue = if ($RunIntegrationTests) { "ON" } else { "OFF" }

$configureArguments = @(
    "-S", $sourceDirectory,
    "-B", $buildDirectory,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DCMAKE_INSTALL_PREFIX=$InstallPrefix",
    "-DCMAKE_PREFIX_PATH=$prefixPath",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL",
    "-DCMAKE_CXX_FLAGS=/utf-8",
    "-DROCM_PATH=$RuntimeRoot",
    "-DROCPROFILER_DEFAULT_ROCM_PATH=$InstallPrefix",
    "-DPython3_EXECUTABLE=$($layout.Python)",
    "-DROCPROFILER_PYTHON_VERSIONS=$pythonVersion",
    "-DROCPROFILER_BUILD_WINDOWS_MINIMAL=ON",
    "-DROCPROFILER_BUILD_WINDOWS_TESTS=ON",
    "-DROCPROFILER_BUILD_WINDOWS_INTEGRATION_TESTS=$integrationTestsValue",
    "-DROCPROFILER_WINDOWS_TEST_VENV_ROOT=$($layout.VenvRoot)",
    "-DROCPROFILER_WINDOWS_TEST_RUNTIME_ROOT=$RuntimeRoot",
    "-DROCPROFILER_WINDOWS_TEST_INSTALL_PREFIX=$InstallPrefix",
    "-DROCPROFILER_WINDOWS_TEST_OUTPUT_ROOT=$(Join-Path $validationDirectory 'installed-prefix')",
    "-DROCPROFILER_WINDOWS_HSA_INCLUDE_DIR=$(Join-Path $RuntimeRoot 'include/hsa')",
    "-DROCPROFILER_WINDOWS_HSA_LIBRARY=$(Join-Path $RuntimeRoot 'lib/amdhip64.lib')",
    "-DROCPROFILER_BUILD_AQLPROFILE=ON",
    "-DROCPROFILER_BUILD_TESTS=OFF",
    "-DROCPROFILER_BUILD_SAMPLES=OFF",
    "-DROCPROFILER_BUILD_GOTCHA=OFF",
    "-DROCPROFILER_BUILD_PYBIND11=OFF",
    "-DROCPROFILER_BUILD_SQLITE3=OFF",
    "-DROCPROFILER_BUILD_GHC_FS=OFF",
    "-DROCPROFILER_BUILD_ABSEIL=ON",
    "-DROCPROFILER_BUILD_FMT=ON",
    "-DROCPROFILER_BUILD_YAML_CPP=ON",
    "-DROCPROFILER_BUILD_WERROR=OFF"
)
Invoke-NativeCommand -FilePath "cmake.exe" -ArgumentList $configureArguments

if ($ConfigureOnly) {
    Write-Host "ROCProfiler SDK configuration completed: $buildDirectory"
    exit 0
}

$buildArguments = @("--build", $buildDirectory, "--config", $BuildType, "--parallel")
if (-not $BuildAllTargets) {
    $buildTargets = @(
        "rocprofiler-sdk-shared-library",
        "rocprofiler-sdk-roctx-shared-library",
        "rocprofiler-sdk-tool",
        "rocprofv3-list-avail",
        "rocprofiler-sdk-windows-registration",
        "rocprofiler-sdk-windows-registration-reentrancy",
        "rocprofiler-sdk-windows-registration-concurrent",
        "rocprofiler-sdk-windows-registration-multi-client"
    )
    if ($RunIntegrationTests) {
        $buildTargets += @(
            "rocprofiler-sdk-windows-hip-workload",
            "rocprofiler-sdk-windows-dispatch-analysis-workload",
            "rocprofiler-sdk-windows-roctx-workload"
        )
    }
    $buildArguments += @("--target") + $buildTargets
}
Invoke-NativeCommand -FilePath "cmake.exe" -ArgumentList $buildArguments

Invoke-NativeCommand -FilePath "ctest.exe" -ArgumentList @(
    "--test-dir", $buildDirectory,
    "-C", $BuildType,
    "-R", "^rocprofiler-sdk\.windows\.(rocprofv3\.(unit|process|pmc-process)|dispatch-analysis-contract|pmc-contract|aqlprofile-mirror|job-object|registration\.(normal|late|reentrancy|concurrent|multi-client|finalization-failure))$",
    "-V",
    "--output-on-failure",
    "--no-tests=error",
    "--output-log", (Join-Path $validationDirectory "rocprofv3-unit.txt")
)
if ($RunIntegrationTests) {
    Invoke-NativeCommand -FilePath "ctest.exe" -ArgumentList @(
        "--test-dir", $buildDirectory,
        "-C", $BuildType,
        "-R", "^rocprofiler-sdk\.windows\.(availability|baseline|kernel-trace|dispatch-analysis-contract|hip-trace|hip-graph|hip-marker|roctx-trace|no-overwrite|hsa-barrier)\.(execute|validate)$",
        "-V",
        "--output-on-failure",
        "--no-tests=error",
        "--output-log", (Join-Path $validationDirectory "windows-safe-integration.txt")
    )
}

if ($SkipInstall) {
    Write-Host "ROCProfiler SDK build completed without installation."
    exit 0
}

# Remove only files owned by this component. The active ROCM_PATH remains in
# place, while package-cache hard links are broken before CMake writes files.
foreach ($relativePath in @(
    "bin/rocprofiler-sdk.dll",
    "bin/rocprofiler-sdk-roctx.dll",
    "bin/rocprofiler-sdk-tool.dll",
    "bin/rocprofv3-list-avail.dll",
    "bin/rocprofiler-sdk",
    "bin/rocprofv3",
    "bin/rocprofv3-avail",
    "bin/rocprofv3.cmd",
    "bin/rocprofv3-avail.cmd",
    "bin/_rocprofv3_windows.py",
    "bin/_rocprofv3_windows_job.py",
    "lib/rocprofiler-sdk.lib",
    "lib/rocprofiler-sdk-roctx.lib",
    "lib/rocprofiler-sdk-tool.lib",
    "lib/rocprofiler-sdk",
    "lib/cmake/rocprofiler-sdk",
    "include/rocprofiler-sdk",
    "include/rocprofiler-sdk-roctx",
    "include/rocprofiler-sdk-rocattach",
    "include/rocprofiler-sdk-rocpd",
    "share/rocprofiler-sdk",
    "share/rocprofiler-sdk-rocpd",
    "share/doc/rocprofiler-sdk",
    "share/doc/rocprofiler-sdk-tests",
    "share/modulefiles/rocprofiler-sdk",
    "lib/python3/site-packages/rocprofv3"
)) {
    Remove-WindowsInstallEntry -Path (Join-Path $InstallPrefix $relativePath)
}

foreach ($component in @("core", "tools", "roctx", "development")) {
    Invoke-NativeCommand -FilePath "cmake.exe" -ArgumentList @(
        "--install", $buildDirectory,
        "--config", $BuildType,
        "--component", $component
    )
}

$installBin = Join-Path $InstallPrefix "bin"
$listAvailDestination = Join-Path $installBin "rocprofv3-list-avail.dll"
$requiredPaths = @(
    (Join-Path $installBin "rocprofiler-sdk.dll"),
    (Join-Path $installBin "rocprofiler-sdk-roctx.dll"),
    (Join-Path $installBin "rocprofiler-sdk-tool.dll"),
    $listAvailDestination,
    (Join-Path $installBin "rocprofv3"),
    (Join-Path $installBin "rocprofv3-avail"),
    (Join-Path $installBin "rocprofv3.cmd"),
    (Join-Path $installBin "rocprofv3-avail.cmd"),
    (Join-Path $installBin "_rocprofv3_windows.py"),
    (Join-Path $installBin "_rocprofv3_windows_job.py"),
    (Join-Path $InstallPrefix "include/rocprofiler-sdk/rocprofiler.h"),
    (Join-Path $InstallPrefix "include/rocprofiler-sdk-roctx/roctx.h"),
    (Join-Path $InstallPrefix "lib/rocprofiler-sdk.lib"),
    (Join-Path $InstallPrefix "lib/rocprofiler-sdk-roctx.lib"),
    (Join-Path $InstallPrefix "lib/rocprofiler-sdk-tool.lib"),
    (Join-Path $InstallPrefix "lib/rocprofiler-sdk/rocprofv3-list-avail.lib"),
    (Join-Path $InstallPrefix "lib/cmake/rocprofiler-sdk/rocprofiler-sdk-config.cmake"),
    (Join-Path $InstallPrefix "lib/python3/site-packages/rocprofv3/__init__.py"),
    (Join-Path $InstallPrefix "lib/python3/site-packages/rocprofv3/avail.py"),
    (Join-Path $InstallPrefix "share/rocprofiler-sdk/config.yaml")
)
foreach ($requiredPath in $requiredPaths) {
    if (-not (Test-Path $requiredPath -PathType Leaf)) {
        throw "The installed Windows ROCProfiler SDK is incomplete: $requiredPath"
    }
}

# Keep the split package's executable runtime and metric files synchronized. A
# Windows package uses independent files rather than POSIX symlinks.
if ($publishActiveSplit) {
    Remove-WindowsInstallEntry -Path (Join-Path $layout.CoreRoot "bin/rocprofiler-sdk")
    foreach ($relativePath in @(
        "bin/rocprofiler-sdk.dll",
        "bin/rocprofiler-sdk-roctx.dll",
        "bin/rocprofiler-sdk-tool.dll",
        "bin/rocprofv3-list-avail.dll"
    )) {
        Install-WindowsFile -Source (Join-Path $InstallPrefix $relativePath) `
            -Destination (Join-Path $layout.CoreRoot $relativePath)
    }
    Install-WindowsDirectory -Source (Join-Path $InstallPrefix "share/rocprofiler-sdk") `
        -Destination (Join-Path $layout.CoreRoot "share/rocprofiler-sdk")
}

# CMake installs the Python backend beside the Windows launcher. Keep the
# normal venv import package current as well, but never write it for a custom
# comparison prefix.
if ($publishActiveSplit) {
    Install-WindowsDirectory `
        -Source (Join-Path $InstallPrefix "lib/python3/site-packages/rocprofv3") `
        -Destination (Join-Path $layout.SitePackages "rocprofv3")

    $wrapperDirectory = Join-Path $validationDirectory "wrappers"
    New-Item -ItemType Directory -Force -Path $wrapperDirectory | Out-Null
    foreach ($command in @("rocprofv3", "rocprofv3-avail")) {
        $wrapper = Join-Path $wrapperDirectory "$command.cmd"
        Set-Content -Path $wrapper -Encoding ascii -NoNewline -Value @"
@echo off
call "%~dp0..\Lib\site-packages\_rocm_sdk_devel\bin\$command.cmd" %*
exit /b %ERRORLEVEL%
"@
        Install-WindowsFile -Source $wrapper `
            -Destination (Join-Path $layout.ScriptsRoot "$command.cmd")
    }
}

$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if ($dumpbin) {
    $dumpbinDependents = & $dumpbin.Source /dependents (Join-Path $installBin "rocprofiler-sdk.dll")
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin.exe /dependents failed for the installed ROCProfiler SDK"
    }
    Set-Content -Path (Join-Path $validationDirectory "rocprofiler-sdk-dependents.txt") `
        -Value $dumpbinDependents -Encoding utf8
    $dumpbinExports = & $dumpbin.Source /exports (Join-Path $installBin "rocprofiler-sdk.dll")
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin.exe /exports failed for the installed ROCProfiler SDK"
    }
    Set-Content -Path (Join-Path $validationDirectory "rocprofiler-sdk-exports.txt") `
        -Value $dumpbinExports -Encoding utf8
}

Invoke-NativeCommand -FilePath "ctest.exe" -ArgumentList @(
    "--test-dir", $buildDirectory,
    "-C", $BuildType,
    "-R", "^rocprofiler-sdk\.windows\.build-prefix$",
    "-V",
    "--output-on-failure",
    "--no-tests=error",
    "--output-log", (Join-Path $validationDirectory "windows-build-prefix.txt")
)
Invoke-NativeCommand -FilePath "ctest.exe" -ArgumentList @(
    "--test-dir", $buildDirectory,
    "-C", $BuildType,
    "-R", "^rocprofiler-sdk\.windows\.installed-prefix$",
    "-V",
    "--output-on-failure",
    "--no-tests=error",
    "--output-log", (Join-Path $validationDirectory "windows-installed-prefix.txt")
)
if ($RunIntegrationTests) {
    Invoke-NativeCommand -FilePath "ctest.exe" -ArgumentList @(
        "--test-dir", $buildDirectory,
        "-C", $BuildType,
        "-R", "^rocprofiler-sdk\.windows\.installed-prefix\.hip$",
        "-V",
        "--output-on-failure",
        "--no-tests=error",
        "--output-log", (Join-Path $validationDirectory "windows-installed-prefix-hip.txt")
    )
}

Write-Host ""
Write-Host "ROCProfiler SDK installed in $InstallPrefix."
Write-Host "SDK DLL:          $(Join-Path $installBin 'rocprofiler-sdk.dll')"
Write-Host "Registration DLL: $(Join-Path $installBin 'rocprofiler-register.dll')"
Write-Host "Availability DLL: $listAvailDestination"
Write-Host "CLI wrapper:      $(Join-Path $installBin 'rocprofv3.cmd')"
Write-Host "Validation:       $validationDirectory"
