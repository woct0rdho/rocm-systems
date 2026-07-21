param(
    [string]$VenvPath = $env:VIRTUAL_ENV,
    [string]$BuildDirectory = "",
    [string]$InstallPrefix = "",
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

$sourceDirectory = Join-Path $repositoryRoot "projects/rocprofiler-register"
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repositoryRoot "build/rocprofiler-register"
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

Import-VisualStudioBuildEnvironment
foreach ($tool in @("cmake.exe", "ninja.exe")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool is required and was not found on PATH."
    }
}

if ($Clean -and (Test-Path $buildDirectory)) {
    Write-Host "Removing $buildDirectory"
    Remove-Item -Recurse -Force $buildDirectory
}
New-Item -ItemType Directory -Force -Path $buildDirectory | Out-Null

$configureArguments = @(
    "-S", $sourceDirectory,
    "-B", $buildDirectory,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DCMAKE_INSTALL_PREFIX=$InstallPrefix",
    "-DCMAKE_INSTALL_LIBDIR=lib",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
    "-DBUILD_SHARED_LIBS=ON",
    "-DROCPROFILER_REGISTER_BUILD_TESTS=OFF",
    "-DROCPROFILER_REGISTER_BUILD_SAMPLES=OFF",
    "-DROCPROFILER_REGISTER_BUILD_DOCS=OFF",
    "-DPython3_EXECUTABLE=$($layout.Python)"
)
Invoke-NativeCommand -FilePath "cmake.exe" -ArgumentList $configureArguments

if ($ConfigureOnly) {
    Write-Host "rocprofiler-register configuration completed: $buildDirectory"
    exit 0
}

Invoke-NativeCommand -FilePath "cmake.exe" -ArgumentList @(
    "--build", $buildDirectory,
    "--config", $BuildType,
    "--parallel"
)

if ($SkipInstall) {
    Write-Host "rocprofiler-register build completed without installation."
    exit 0
}

foreach ($relativePath in @(
    "bin/rocprofiler-register.dll",
    "lib/rocprofiler-register.lib",
    "include/rocprofiler-register",
    "lib/cmake/rocprofiler-register",
    "share/doc/rocprofiler-register",
    "share/rocprofiler-register",
    "share/modulefiles/rocprofiler-register"
)) {
    Remove-WindowsInstallEntry -Path (Join-Path $InstallPrefix $relativePath)
}
Invoke-NativeCommand -FilePath "cmake.exe" -ArgumentList @(
    "--install", $buildDirectory,
    "--config", $BuildType,
    "--component", "core"
)

foreach ($relativePath in @(
    "bin/rocprofiler-register.dll",
    "lib/rocprofiler-register.lib",
    "include/rocprofiler-register/rocprofiler-register.h",
    "lib/cmake/rocprofiler-register/rocprofiler-register-config.cmake"
)) {
    $installedPath = Join-Path $InstallPrefix $relativePath
    if (-not (Test-Path $installedPath -PathType Leaf)) {
        throw "The installed rocprofiler-register package is incomplete: $installedPath"
    }
}

$installedDll = Join-Path $InstallPrefix "bin/rocprofiler-register.dll"
if ($publishActiveSplit -and (Test-Path $layout.CoreRoot -PathType Container)) {
    Install-WindowsFile -Source $installedDll `
        -Destination (Join-Path $layout.CoreBin "rocprofiler-register.dll")
}

Write-Host ""
Write-Host "rocprofiler-register installed in $InstallPrefix."
Write-Host "Registration DLL: $installedDll"
if ($publishActiveSplit) {
    Write-Host "Core DLL:         $(Join-Path $layout.CoreBin 'rocprofiler-register.dll')"
}
