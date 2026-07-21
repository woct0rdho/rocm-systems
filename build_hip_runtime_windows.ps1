param(
    [string]$VenvPath,
    [string]$BuildDirectory,
    [string]$InstallPrefix,
    [string]$RocprofilerRegisterRoot,
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$BuildType,
    [switch]$Clean,
    [switch]$ConfigureOnly,
    [switch]$SkipInstall
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$entryPoint = Join-Path $PSScriptRoot "projects/clr/scripts/build_windows_runtime.ps1"
Write-Warning "build_hip_runtime_windows.ps1 is a compatibility shim; use $entryPoint"
& $entryPoint @PSBoundParameters
