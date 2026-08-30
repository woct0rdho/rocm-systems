[CmdletBinding()]
param(
  [string]$VenvPath = 'C:\venv_torch',
  [string]$ClrBuildDirectory = 'C:\rocm-systems\build\clr-hip',
  [string]$RuntimeDll = '',
  [string]$OutputDirectory = 'C:\rocm-systems\build\rocr-scratch-recovery\fixed',
  [switch]$Run
)

$ErrorActionPreference = 'Stop'
$repoRoot = $PSScriptRoot
$source = Join-Path $repoRoot 'rocr_scratch_recovery_test.cpp'
$hsaInclude = Join-Path $VenvPath 'Lib\site-packages\_rocm_sdk_devel\include'
$rocrInclude = Join-Path $repoRoot 'projects\rocr-runtime\runtime\hsa-runtime\inc'
$hipLibDirectory = Join-Path $ClrBuildDirectory 'hipamd\lib'
$hipImportLibrary = Join-Path $hipLibDirectory 'amdhip64.lib'
$packageBin = Join-Path $VenvPath 'Lib\site-packages\_rocm_sdk_devel\bin'
$packageLib = Join-Path $VenvPath 'Lib\site-packages\_rocm_sdk_devel\lib'

if ([string]::IsNullOrWhiteSpace($RuntimeDll)) {
  $RuntimeDll = Join-Path $ClrBuildDirectory 'hipamd\src\amdhip64_7.dll'
}

$cl = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\bin\HostX64\x64\cl.exe'
if (-not (Test-Path $cl)) {
  $cl = (Get-Command cl.exe -ErrorAction Stop).Source
}

foreach ($path in @($source, $hsaInclude, $rocrInclude, $hipImportLibrary, $RuntimeDll)) {
  if (-not (Test-Path $path)) {
    throw "Required path does not exist: $path"
  }
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$exe = Join-Path $OutputDirectory 'rocr_scratch_recovery_test.exe'
$obj = Join-Path $OutputDirectory 'rocr_scratch_recovery_test.obj'
$compileArgs = @(
  '/nologo',
  '/std:c++20',
  '/EHsc',
  "/I$hsaInclude",
  "/I$rocrInclude",
  $source,
  "/Fo:$obj",
  "/Fe:$exe",
  '/link',
  "/LIBPATH:$hipLibDirectory",
  'amdhip64.lib'
)

& $cl @compileArgs
if ($LASTEXITCODE -ne 0) {
  throw "cl.exe failed with exit code $LASTEXITCODE"
}

Copy-Item $RuntimeDll (Join-Path $OutputDirectory 'amdhip64_7.dll') -Force
$runtimeHash = (Get-FileHash (Join-Path $OutputDirectory 'amdhip64_7.dll') -Algorithm SHA256).Hash
Write-Output "built=$exe"
Write-Output "runtime=$RuntimeDll"
Write-Output "runtime_sha256=$runtimeHash"

if ($Run) {
  $env:PATH = "$packageBin;$packageLib;" + $env:PATH
  & $exe
  if ($LASTEXITCODE -ne 0) {
    throw "rocr_scratch_recovery_test.exe failed with exit code $LASTEXITCODE"
  }
}
