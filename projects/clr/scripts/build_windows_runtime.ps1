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

$sourceDirectory = Join-Path $repositoryRoot "projects/clr"
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repositoryRoot "build/clr-hip"
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
$dependencyRoot = Get-WindowsDependencyRoot -InstallPrefix $InstallPrefix `
    -ActiveDevelRoot $layout.DevelRoot
if ([string]::IsNullOrWhiteSpace($RocprofilerRegisterRoot)) {
    $RocprofilerRegisterRoot = $dependencyRoot
}
$RocprofilerRegisterRoot = [IO.Path]::GetFullPath($RocprofilerRegisterRoot)

$interopRoot = Join-Path $repositoryRoot "shared/amdgpu-windows-interop"
$wkmiLibrary = Join-Path $interopRoot "wkmi/win/lib/wkmi.lib"
$wkmiExpectedMd5 = "D5D5D50AA73F85886029E3DCBCE7F03F"
$wkmiExpectedSize = 448252
$rocprofilerRegisterConfig = Join-Path $RocprofilerRegisterRoot `
    "lib/cmake/rocprofiler-register/rocprofiler-register-config.cmake"
$system32Hip = "C:/Windows/System32/amdhip64_7.dll"
$system32Hash = $null
$currentSystem32Hash = $null
if (Test-Path $system32Hip -PathType Leaf) {
    $system32Hash = (Get-FileHash $system32Hip -Algorithm SHA256).Hash.ToLowerInvariant()
}

Import-VisualStudioBuildEnvironment
foreach ($tool in @("cmake.exe", "ninja.exe", "ctest.exe", "git.exe")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool is required and was not found on PATH."
    }
}

if (-not (Test-Path $wkmiLibrary -PathType Leaf)) {
    throw @"
The DVC-pinned WKMI archive is missing:
$wkmiLibrary
Materialize only that repository-pinned object before building, for example:
$($layout.Python) $(Join-Path $repositoryRoot 'fetch_wkmi_dvc.py')
This build script never downloads or substitutes WKMI or PAL libraries.
"@
}
$wkmiFile = Get-Item $wkmiLibrary
$wkmiMd5 = (Get-FileHash $wkmiLibrary -Algorithm MD5).Hash
if ($wkmiFile.Length -ne $wkmiExpectedSize -or $wkmiMd5 -ne $wkmiExpectedMd5) {
    throw "WKMI does not match the repository pointer: size=$($wkmiFile.Length) md5=$wkmiMd5"
}
if (-not (Test-Path $rocprofilerRegisterConfig -PathType Leaf)) {
    throw "The rocprofiler-register installation is incomplete: $rocprofilerRegisterConfig"
}

if ($Clean -and (Test-Path $buildDirectory)) {
    Write-Host "Removing $buildDirectory"
    Remove-Item -Recurse -Force $buildDirectory
}
New-Item -ItemType Directory -Force -Path $buildDirectory | Out-Null

$prefixPath = if ($publishActiveSplit) {
    "$RocprofilerRegisterRoot;$dependencyRoot;$($layout.CoreRoot)"
} else {
    "$RocprofilerRegisterRoot;$dependencyRoot"
}
$llvmDirectory = Join-Path $layout.DevelLib "cmake/llvm"
$clangDirectory = Join-Path $layout.DevelLib "cmake/clang"
$configureArguments = @(
    "-S", $sourceDirectory,
    "-B", $buildDirectory,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DCMAKE_INSTALL_PREFIX=$InstallPrefix",
    "-DCMAKE_PREFIX_PATH=$prefixPath",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
    "-DCMAKE_CXX_FLAGS=/utf-8",
    "-DROCM_PATH=$dependencyRoot",
    "-DLLVM_DIR=$llvmDirectory",
    "-DClang_DIR=$clangDirectory",
    "-DPython3_EXECUTABLE=$($layout.Python)",
    "-DHIP_PLATFORM=amd",
    "-DHIP_COMMON_DIR=$(Join-Path $repositoryRoot 'projects/hip')",
    "-DCLR_BUILD_HIP=ON",
    "-DBUILD_SHARED_LIBS=ON",
    "-DAMD_COMPUTE_WIN=$interopRoot",
    "-DROCCLR_ENABLE_HSA=ON",
    "-DROCCLR_ENABLE_PAL=OFF",
    "-DROCCLR_BUILD_WINDOWS_TESTS=ON",
    "-DUSE_PROF_API=OFF",
    "-DHIP_ENABLE_ROCPROFILER_REGISTER=ON",
    "-D__HIP_ENABLE_PCH=OFF",
    "-DROCM_KPACK_ENABLED=ON",
    "-DIMAGE_SUPPORT=OFF",
    "-DPC_SAMPLING_SUPPORT=OFF"
)
Invoke-NativeCommand -FilePath "cmake.exe" -ArgumentList $configureArguments

if ($ConfigureOnly) {
    Write-Host "HIP/ROCr Windows configuration completed: $buildDirectory"
    exit 0
}

Invoke-NativeCommand -FilePath "cmake.exe" -ArgumentList @(
    "--build", $buildDirectory,
    "--config", $BuildType,
    "--target", "amdhip64", "clr-windows-registration-profiler",
        "clr-windows-registration-test",
    "--parallel"
)

$validationDirectory = Join-Path $buildDirectory "validation"
New-Item -ItemType Directory -Force -Path $validationDirectory | Out-Null
foreach ($test in @(
    @{ Pattern = "^clr\.windows\.runtime-binary$"; Log = "hip-runtime-binary-test.txt" },
    @{ Pattern = "^clr\.windows\.registration\.normal$"; Log = "registration-normal-test.txt" },
    @{ Pattern = "^clr\.windows\.registration\.late$"; Log = "registration-late-test.txt" }
)) {
    Invoke-NativeCommand -FilePath "ctest.exe" -ArgumentList @(
        "--test-dir", $buildDirectory,
        "-C", $BuildType,
        "-R", $test.Pattern,
        "-V",
        "--output-on-failure",
        "--no-tests=error",
        "--output-log", (Join-Path $validationDirectory $test.Log)
    )
}

if ($SkipInstall) {
    Write-Host "HIP/ROCr Windows build completed without installation."
    exit 0
}

# CLR's generated Windows install script contains absolute active-prefix paths
# and removes the prefix-wide include directory. Publish only the HIP runtime
# artifacts owned by this build instead of invoking that unsafe install graph.
$candidateDll = Join-Path $buildDirectory "hipamd/src/amdhip64_7.dll"
$candidateLibrary = Join-Path $buildDirectory "hipamd/lib/amdhip64.lib"
foreach ($requiredFile in @($candidateDll, $candidateLibrary)) {
    if (-not (Test-Path $requiredFile -PathType Leaf)) {
        throw "Expected HIP build output was not found: $requiredFile"
    }
}

Install-WindowsFile -Source $candidateDll `
    -Destination (Join-Path $InstallPrefix "bin/amdhip64_7.dll")
Install-WindowsFile -Source $candidateLibrary `
    -Destination (Join-Path $InstallPrefix "lib/amdhip64.lib")

$candidatePdb = Join-Path $buildDirectory "hipamd/src/amdhip64_7.pdb"
if (Test-Path $candidatePdb -PathType Leaf) {
    Install-WindowsFile -Source $candidatePdb `
        -Destination (Join-Path $InstallPrefix "bin/amdhip64_7.pdb")
}

if ($publishActiveSplit) {
    Install-WindowsFile -Source $candidateDll `
        -Destination (Join-Path $layout.CoreBin "amdhip64_7.dll")
}

$kpackValidation = Join-Path $validationDirectory "hip-runtime-kpack.txt"
$torchKpackDirectory = Join-Path $layout.SitePackages "torch/.kpack"
$torchKpacks = @()
if (Test-Path $torchKpackDirectory -PathType Container) {
    $torchKpacks = @(Get-ChildItem $torchKpackDirectory -Filter "*.kpack" -File)
}
if ($publishActiveSplit -and $torchKpacks.Count -gt 0) {
    $kpackProbe = @"
import torch
assert torch.cuda.is_available(), "PyTorch did not find a HIP device"
x = torch.arange(4, device="cuda")
y = (x + 1).cpu().tolist()
torch.cuda.synchronize()
assert y == [1, 2, 3, 4], y
print(f"windows_hip_kpack=passed device={torch.cuda.get_device_name(0)!r}")
"@
    Invoke-NativeCommand -FilePath $layout.Python -ArgumentList @("-c", $kpackProbe)
    Set-Content -Path $kpackValidation -Value @(
        "windows_hip_kpack=passed",
        "archives=$($torchKpacks.Count)",
        "torch_kpack_directory=$torchKpackDirectory"
    ) -Encoding utf8
} else {
    Set-Content -Path $kpackValidation -Value @(
        "windows_hip_kpack=skipped",
        "active_split=$publishActiveSplit",
        "archives=$($torchKpacks.Count)",
        "torch_kpack_directory=$torchKpackDirectory"
    ) -Encoding utf8
}

if ($system32Hash) {
    $currentSystem32Hash = (Get-FileHash $system32Hip -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($currentSystem32Hash -ne $system32Hash) {
        throw "System32 HIP runtime changed during the build: $system32Hip"
    }
}

$runtimeManifest = Join-Path $validationDirectory "hip-runtime-install.txt"
$manifestRows = @(
    "windows_hip_runtime_install=passed",
    "prefix=$InstallPrefix"
)
if ($system32Hash) {
    $manifestRows += "system32_hip_path=$system32Hip"
    $manifestRows += "system32_sha256_before=$system32Hash"
    $manifestRows += "system32_sha256_after=$currentSystem32Hash"
    $manifestRows += "system32_unchanged=$($currentSystem32Hash -eq $system32Hash)"
}
foreach ($artifact in @(
    (Join-Path $InstallPrefix "bin/amdhip64_7.dll"),
    (Join-Path $InstallPrefix "lib/amdhip64.lib"),
    (Join-Path $InstallPrefix "bin/amdhip64_7.pdb")
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
        "/dependents", (Join-Path $InstallPrefix "bin/amdhip64_7.dll")
    )
}

Write-Host ""
Write-Host "HIP/ROCr Windows runtime installed in $InstallPrefix."
Write-Host "Runtime DLL: $(Join-Path $InstallPrefix 'bin/amdhip64_7.dll')"
Write-Host "Manifest:    $runtimeManifest"
