param(
    [string]$VenvPath = $env:VIRTUAL_ENV,
    [string]$BuildDirectory = "",
    [string]$InstallPrefix = "",
    [string]$RocprofilerRegisterRoot = "",
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$BuildType = "RelWithDebInfo",
    [switch]$Clean,
    [switch]$ConfigureOnly,
    [switch]$SkipInstall
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../../.."))
. (Join-Path $repositoryRoot "tools/windows-build/common.ps1")

$sourceDirectory = Join-Path $repositoryRoot "projects/rocr-runtime"
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repositoryRoot "build/rocr-runtime"
}
$layout = Get-RocmVenvLayout -VenvPath $VenvPath
if ([string]::IsNullOrWhiteSpace($InstallPrefix)) {
    $InstallPrefix = $env:ROCM_PATH
}
if ([string]::IsNullOrWhiteSpace($InstallPrefix)) {
    throw "ROCM_PATH must name the active development package or pass -InstallPrefix."
}
if ([string]::IsNullOrWhiteSpace($RocprofilerRegisterRoot)) {
    $RocprofilerRegisterRoot = $InstallPrefix
}
$buildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$InstallPrefix = [IO.Path]::GetFullPath($InstallPrefix)
$RocprofilerRegisterRoot = [IO.Path]::GetFullPath($RocprofilerRegisterRoot)

$wkmiLibrary = Join-Path $repositoryRoot "shared/amdgpu-windows-interop/wkmi/win/lib/wkmi.lib"
$wkmiExpectedMd5 = "D5D5D50AA73F85886029E3DCBCE7F03F"
$wkmiExpectedSize = 448252
$rocprofilerRegisterConfig = Join-Path $RocprofilerRegisterRoot `
    "lib/cmake/rocprofiler-register/rocprofiler-register-config.cmake"
$rocprofilerRegisterDll = Join-Path $RocprofilerRegisterRoot "bin/rocprofiler-register.dll"
$rocprofilerRegisterLibrary = Join-Path $RocprofilerRegisterRoot "lib/rocprofiler-register.lib"

Import-VisualStudioBuildEnvironment
foreach ($tool in @("cmake.exe", "ninja.exe", "ctest.exe")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool is required and was not found on PATH."
    }
}

if (-not (Test-Path $wkmiLibrary -PathType Leaf)) {
    throw @"
The DVC-pinned WKMI archive is missing:
$wkmiLibrary
Materialize wkmi.lib from its adjacent .dvc pointer before building, for example:
$($layout.Python) $(Join-Path $repositoryRoot 'fetch_wkmi_dvc.py')
This build script never downloads or substitutes the private driver-interface archive.
"@
}
$wkmiFile = Get-Item $wkmiLibrary
$wkmiMd5 = (Get-FileHash $wkmiLibrary -Algorithm MD5).Hash
if ($wkmiFile.Length -ne $wkmiExpectedSize -or $wkmiMd5 -ne $wkmiExpectedMd5) {
    throw "WKMI does not match the repository pointer: size=$($wkmiFile.Length) md5=$wkmiMd5"
}

foreach ($requiredFile in @(
    $rocprofilerRegisterConfig,
    $rocprofilerRegisterDll,
    $rocprofilerRegisterLibrary
)) {
    if (-not (Test-Path $requiredFile -PathType Leaf)) {
        throw "The rocprofiler-register installation is incomplete: $requiredFile"
    }
}

if ($Clean -and (Test-Path $buildDirectory)) {
    Write-Host "Removing $buildDirectory"
    Remove-Item -Recurse -Force $buildDirectory
}
New-Item -ItemType Directory -Force -Path $buildDirectory | Out-Null

$libElfDirectory = Join-Path $repositoryRoot "shared/amdgpu-windows-interop/hsail-compiler/lib/loaders/elf/utils/libelf"
$llvmDirectory = Join-Path $layout.DevelLib "cmake/llvm"
$clangDirectory = Join-Path $layout.DevelLib "cmake/clang"
$configureArguments = @(
    "-S", $sourceDirectory,
    "-B", $buildDirectory,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DCMAKE_INSTALL_PREFIX=$InstallPrefix",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
    "-DCMAKE_CXX_FLAGS=/utf-8",
    "-DBUILD_SHARED_LIBS=ON",
    "-DIMAGE_SUPPORT=OFF",
    "-DPC_SAMPLING_SUPPORT=OFF",
    "-DROCR_BUILD_WINDOWS_TESTS=ON",
    "-DUSE_AMD_LIBELF=yes",
    "-DAMD_LIBELF_PATH=$libElfDirectory",
    "-DLLVM_DIR=$llvmDirectory",
    "-DClang_DIR=$clangDirectory",
    "-DPython3_EXECUTABLE=$($layout.Python)",
    "-DCMAKE_PREFIX_PATH=$RocprofilerRegisterRoot"
)
Invoke-NativeCommand -FilePath "cmake.exe" -ArgumentList $configureArguments

if ($ConfigureOnly) {
    Write-Host "ROCr Windows configuration completed: $buildDirectory"
    exit 0
}

Invoke-NativeCommand -FilePath "cmake.exe" -ArgumentList @(
    "--build", $buildDirectory,
    "--config", $BuildType,
    "--target", "hsa-runtime64", "hsakmt-windows-packet-publication-test", `
        "hsakmt-windows-profiling-adapter-test", "rocr-windows-queue-profiling-test",
    "--parallel"
)

$validationDirectory = Join-Path $buildDirectory "validation"
New-Item -ItemType Directory -Force -Path $validationDirectory | Out-Null
Invoke-NativeCommand -FilePath "ctest.exe" -ArgumentList @(
    "--test-dir", $buildDirectory,
    "-C", $BuildType,
    "-R", "^(hsakmt\.windows\.(packet-publication|profiling-adapter)|rocr\.windows\.(runtime-binary|queue-profiling))$",
    "-V",
    "--output-on-failure",
    "--no-tests=error",
    "--output-log", (Join-Path $validationDirectory "windows-component-tests.txt")
)

if ($SkipInstall) {
    Write-Host "ROCr Windows build completed without installation."
    exit 0
}

$runtimeDll = Join-Path $buildDirectory "rocr/runtime/hsa-runtime64.dll"
$runtimeLibrary = Join-Path $buildDirectory "rocr/archive/hsa-runtime64.lib"
foreach ($requiredFile in @($runtimeDll, $runtimeLibrary)) {
    if (-not (Test-Path $requiredFile -PathType Leaf)) {
        throw "Expected ROCr build output was not found: $requiredFile"
    }
}

Install-WindowsFile -Source $runtimeDll `
    -Destination (Join-Path $InstallPrefix "bin/hsa-runtime64.dll")
Install-WindowsFile -Source $runtimeLibrary `
    -Destination (Join-Path $InstallPrefix "lib/hsa-runtime64.lib")

# Publish the public ROCr and thunk headers as one directory replacement per
# namespace so CMake never writes through package-cache hard links.
foreach ($relativePath in @("include/hsa", "include/hsakmt")) {
    Remove-WindowsInstallEntry -Path (Join-Path $InstallPrefix $relativePath)
}
Invoke-NativeCommand -FilePath "cmake.exe" -ArgumentList @(
    "--install", $buildDirectory,
    "--config", $BuildType,
    "--component", "dev"
)

$runtimePdb = Join-Path $buildDirectory "rocr/runtime/hsa-runtime64.pdb"
if (Test-Path $runtimePdb -PathType Leaf) {
    Install-WindowsFile -Source $runtimePdb `
        -Destination (Join-Path $InstallPrefix "bin/hsa-runtime64.pdb")
}

$runtimeManifest = Join-Path $validationDirectory "runtime-install.txt"
$manifestRows = @(
    "windows_runtime_install=passed",
    "prefix=$InstallPrefix"
)
foreach ($artifact in @(
    (Join-Path $InstallPrefix "bin/hsa-runtime64.dll"),
    (Join-Path $InstallPrefix "lib/hsa-runtime64.lib"),
    (Join-Path $InstallPrefix "bin/hsa-runtime64.pdb"),
    (Join-Path $InstallPrefix "include/hsa/hsa.h"),
    (Join-Path $InstallPrefix "include/hsa/hsa_ext_amd.h"),
    (Join-Path $InstallPrefix "include/hsa/hsa_ven_amd_aqlprofile.h"),
    (Join-Path $InstallPrefix "include/hsakmt/hsakmt.h")
)) {
    if (Test-Path $artifact -PathType Leaf) {
        $relativePath = [IO.Path]::GetRelativePath($InstallPrefix, $artifact).Replace('\', '/')
        $file = Get-Item $artifact
        $sha256 = (Get-FileHash $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
        $manifestRows += "path=$relativePath size=$($file.Length) sha256=$sha256"
    }
}
Set-Content -Path $runtimeManifest -Value $manifestRows -Encoding utf8

$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if ($dumpbin) {
    Invoke-NativeCommand -FilePath $dumpbin.Source -ArgumentList @(
        "/dependents", (Join-Path $InstallPrefix "bin/hsa-runtime64.dll")
    )
}

Write-Host ""
Write-Host "ROCr Windows runtime installed in $InstallPrefix."
Write-Host "Runtime DLL: $(Join-Path $InstallPrefix 'bin/hsa-runtime64.dll')"
Write-Host "Manifest:    $runtimeManifest"
