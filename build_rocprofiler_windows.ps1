param(
    [string]$VenvPath,
    [string]$BuildDirectory,
    [string]$InstallPrefix,
    [string]$RuntimeRoot,
    [string]$RocprofilerRegisterRoot,
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$BuildType,
    [switch]$Clean,
    [switch]$ConfigureOnly,
    [switch]$SkipInstall,
    [switch]$BuildAllTargets,
    [switch]$RunIntegrationTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$entryPoint = Join-Path $PSScriptRoot "projects/rocprofiler-sdk/scripts/build_windows.ps1"
Write-Warning "build_rocprofiler_windows.ps1 is a compatibility shim; use $entryPoint"
& $entryPoint @PSBoundParameters
