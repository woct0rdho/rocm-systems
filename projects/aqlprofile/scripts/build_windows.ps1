param(
    [string]$VenvPath = $env:VIRTUAL_ENV,
    [string]$BuildDirectory = "",
    [string]$InstallPrefix = "",
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$BuildType = "RelWithDebInfo",
    [ValidateSet("MultiThreaded", "MultiThreadedDLL")]
    [string]$MsvcRuntime = "MultiThreadedDLL",
    [string]$Amdhip64LibDirectory = "",
    [string]$Amdhip64RuntimePath = "",
    [string]$HsaIncludeDirectory = "",
    [switch]$Clean,
    [switch]$ConfigureOnly,
    [switch]$SkipInstall,
    [switch]$BuildProbe,
    [switch]$SkipLoaderProbe
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../../.."))
. (Join-Path $repositoryRoot "tools/windows-build/common.ps1")

$sourceDirectory = Join-Path $repositoryRoot "projects/aqlprofile"
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repositoryRoot "build/aqlprofile"
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
if ([string]::IsNullOrWhiteSpace($Amdhip64LibDirectory)) {
    $Amdhip64LibDirectory = Join-Path $dependencyRoot "lib"
}
$Amdhip64LibDirectory = [IO.Path]::GetFullPath($Amdhip64LibDirectory)
if ([string]::IsNullOrWhiteSpace($Amdhip64RuntimePath)) {
    $Amdhip64RuntimePath = Join-Path $dependencyRoot "bin/amdhip64_7.dll"
}
$Amdhip64RuntimePath = [IO.Path]::GetFullPath($Amdhip64RuntimePath)
if ([string]::IsNullOrWhiteSpace($HsaIncludeDirectory)) {
    $HsaIncludeDirectory = Join-Path $dependencyRoot "include/hsa"
}
$HsaIncludeDirectory = [IO.Path]::GetFullPath($HsaIncludeDirectory)

Import-VisualStudioBuildEnvironment
foreach ($tool in @("cmake.exe", "ninja.exe", "ctest.exe")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool is required and was not found on PATH."
    }
}

if (-not (Test-Path $Amdhip64RuntimePath -PathType Leaf)) {
    throw "The HIP runtime required by AQL Profile was not found: $Amdhip64RuntimePath"
}
if (-not (Test-Path (Join-Path $Amdhip64LibDirectory "amdhip64.lib") -PathType Leaf)) {
    throw "The HIP import library required by AQL Profile was not found: $(Join-Path $Amdhip64LibDirectory 'amdhip64.lib')"
}
if (-not (Test-Path (Join-Path $HsaIncludeDirectory "hsa.h") -PathType Leaf)) {
    throw "The HSA headers required by AQL Profile were not found: $HsaIncludeDirectory"
}

if ($Clean -and (Test-Path $buildDirectory)) {
    Write-Host "Removing $buildDirectory"
    Remove-Item -Recurse -Force $buildDirectory
}
New-Item -ItemType Directory -Force -Path $buildDirectory | Out-Null

$buildProbeValue = if ($BuildProbe) { "ON" } else { "OFF" }
$configureArguments = @(
    "-S", $sourceDirectory,
    "-B", $buildDirectory,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DCMAKE_INSTALL_PREFIX=$InstallPrefix",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=$MsvcRuntime",
    "-DAQLPROFILE_BUILD_TESTS=OFF",
    "-DAQLPROFILE_INSTALL_TESTS=OFF",
    "-DAQLPROFILE_BUILD_WINDOWS_PROBE=$buildProbeValue",
    "-DAQLPROFILE_WINDOWS_HSA_RUNTIME=$Amdhip64RuntimePath",
    "-DAQLPROFILE_HSA_INCLUDE_DIR=$HsaIncludeDirectory",
    "-DAMDHIP64_LIB_DIR=$Amdhip64LibDirectory"
)
Invoke-NativeCommand -FilePath "cmake.exe" -ArgumentList $configureArguments

if ($ConfigureOnly) {
    Write-Host "AQL Profile configuration completed: $buildDirectory"
    exit 0
}

Invoke-NativeCommand -FilePath "cmake.exe" -ArgumentList @(
    "--build", $buildDirectory,
    "--config", $BuildType,
    "--parallel"
)

$validationDirectory = Join-Path $buildDirectory "validation"
if ($BuildProbe) {
    New-Item -ItemType Directory -Force -Path $validationDirectory | Out-Null
    Invoke-NativeCommand -FilePath "ctest.exe" -ArgumentList @(
        "--test-dir", $buildDirectory,
        "-C", $BuildType,
        "-R", "^aqlprofile\.windows\.pmc-packet\.gfx1151$",
        "-V",
        "--output-on-failure",
        "--no-tests=error",
        "--output-log", (Join-Path $validationDirectory "pmc-packet-probe.txt")
    )
}

if ($SkipInstall) {
    Write-Host "AQL Profile build completed without installation."
    exit 0
}

# CMake's file(INSTALL) can overwrite a package-cache hard link in place. Remove
# package-owned destinations first so installation always creates independent files.
foreach ($relativePath in @(
    "bin/hsa-amd-aqlprofile64.dll",
    "lib/hsa-amd-aqlprofile64.lib",
    "include/aqlprofile-sdk",
    "share/doc/hsa-amd-aqlprofile"
)) {
    Remove-WindowsInstallEntry -Path (Join-Path $InstallPrefix $relativePath)
}
foreach ($component in @("runtime", "development")) {
    Invoke-NativeCommand -FilePath "cmake.exe" -ArgumentList @(
        "--install", $buildDirectory,
        "--config", $BuildType,
        "--component", $component
    )
}

if ($BuildProbe -and -not $SkipLoaderProbe) {
    New-Item -ItemType Directory -Force -Path $validationDirectory | Out-Null
    Invoke-NativeCommand -FilePath "ctest.exe" -ArgumentList @(
        "--test-dir", $buildDirectory,
        "-C", $BuildType,
        "-R", "^aqlprofile\.windows\.loader$",
        "-V",
        "--output-on-failure",
        "--no-tests=error",
        "--output-log", (Join-Path $validationDirectory "loader-probe.txt")
    )
}

$installedDll = Join-Path $InstallPrefix "bin/hsa-amd-aqlprofile64.dll"
if (-not (Test-Path $installedDll -PathType Leaf)) {
    $installedDll = Find-NewestFile -Roots @($InstallPrefix, $buildDirectory) `
        -FileName "hsa-amd-aqlprofile64.dll"
    if (-not $installedDll) {
        throw "The build completed, but hsa-amd-aqlprofile64.dll was not installed."
    }
    $installedDll = $installedDll.FullName
}

# Windows package splits keep executable runtime DLLs in the core package. Use
# replacement rather than Copy-Item -Force so a cache hard link is not changed.
if ($publishActiveSplit -and (Test-Path $layout.CoreRoot -PathType Container)) {
    Install-WindowsFile -Source $installedDll `
        -Destination (Join-Path $layout.CoreBin "hsa-amd-aqlprofile64.dll")
}

$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if ($dumpbin) {
    Invoke-NativeCommand -FilePath $dumpbin.Source -ArgumentList @(
        "/dependents", $installedDll
    )
}

Write-Host ""
Write-Host "AQL Profile installed in $InstallPrefix."
Write-Host "Runtime DLL: $installedDll"
if ($publishActiveSplit) {
    Write-Host "Core DLL:    $(Join-Path $layout.CoreBin 'hsa-amd-aqlprofile64.dll')"
}
