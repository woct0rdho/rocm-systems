param(
    [string]$VenvPath,
    [string]$BuildDirectory,
    [string]$InstallPrefix,
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$BuildType,
    [switch]$Clean,
    [switch]$ConfigureOnly,
    [switch]$SkipInstall
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$entryPoint = Join-Path $PSScriptRoot "projects/rocprofiler-register/scripts/build_windows.ps1"
Write-Warning "build_rocprofiler_register_windows.ps1 is a compatibility shim; use $entryPoint"
& $entryPoint @PSBoundParameters
