param(
    [string]$VenvPath,
    [string]$BuildDirectory,
    [string]$InstallPrefix,
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$BuildType,
    [ValidateSet("MultiThreaded", "MultiThreadedDLL")]
    [string]$MsvcRuntime,
    [string]$Amdhip64LibDirectory,
    [string]$Amdhip64RuntimePath,
    [string]$HsaIncludeDirectory,
    [switch]$Clean,
    [switch]$ConfigureOnly,
    [switch]$SkipInstall,
    [switch]$BuildProbe,
    [switch]$SkipLoaderProbe
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$entryPoint = Join-Path $PSScriptRoot "projects/aqlprofile/scripts/build_windows.ps1"
Write-Warning "build_aqlprofile_windows.ps1 is a compatibility shim; use $entryPoint"
& $entryPoint @PSBoundParameters
