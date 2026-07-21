Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter()][string[]]$ArgumentList = @()
    )

    $displayArgs = $ArgumentList | ForEach-Object {
        if ($_ -match '\s') { '"{0}"' -f $_ } else { $_ }
    }
    Write-Host ("> {0} {1}" -f $FilePath, ($displayArgs -join " "))

    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath"
    }
}

function Import-VisualStudioBuildEnvironment {
    if ((Get-Command cl.exe -ErrorAction SilentlyContinue) -and
        (Get-Command link.exe -ErrorAction SilentlyContinue)) {
        return
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/Installer/vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "cl.exe is not on PATH and vswhere.exe was not found. Run from a Visual Studio Developer PowerShell."
    }

    $installationPath = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath | Select-Object -First 1
    if (-not $installationPath) {
        throw "No Visual Studio installation with the x64 C++ toolchain was found."
    }
    $installationPath = $installationPath.Trim()
    if (-not $installationPath) {
        throw "No Visual Studio installation with the x64 C++ toolchain was found."
    }

    $devCmd = Join-Path $installationPath "Common7/Tools/VsDevCmd.bat"
    if (-not (Test-Path $devCmd)) {
        throw "Visual Studio developer environment script was not found: $devCmd"
    }

    $cmdLine = ('"{0}" -no_logo -arch=x64 -host_arch=x64 >nul && set' -f $devCmd)
    $environmentLines = & $env:ComSpec /s /c $cmdLine
    if ($LASTEXITCODE -ne 0) {
        throw "VsDevCmd.bat failed with exit code $LASTEXITCODE"
    }

    foreach ($line in $environmentLines) {
        $separator = $line.IndexOf('=')
        if ($separator -le 0) {
            continue
        }

        $name = $line.Substring(0, $separator)
        if ($name.StartsWith('=')) {
            continue
        }
        $value = $line.Substring($separator + 1)
        [Environment]::SetEnvironmentVariable($name, $value, 'Process')
    }

    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw "Visual Studio environment was imported, but cl.exe is still unavailable."
    }
}

function Get-RocmVenvLayout {
    param([string]$VenvPath)

    $rocmPath = $env:ROCM_PATH
    if (-not $VenvPath) {
        $VenvPath = $env:VIRTUAL_ENV
    }
    if (-not $VenvPath -and $rocmPath) {
        $develRoot = [IO.Path]::GetFullPath($rocmPath)
        if ((Split-Path -Leaf $develRoot) -eq "_rocm_sdk_devel") {
            $sitePackages = Split-Path -Parent $develRoot
            $libDirectory = Split-Path -Parent $sitePackages
            $VenvPath = Split-Path -Parent $libDirectory
        }
    }
    if (-not $VenvPath) {
        throw "Activate the target Python environment or pass -VenvPath."
    }

    $VenvPath = [IO.Path]::GetFullPath($VenvPath)
    $python = Join-Path $VenvPath "Scripts/python.exe"
    if (-not (Test-Path $python -PathType Leaf)) {
        throw "Python was not found in the requested virtual environment: $python"
    }

    $sitePackages = & $python -c "import site; print(next(p for p in site.getsitepackages() if p.lower().endswith('site-packages')))" | Select-Object -First 1
    if (-not $sitePackages) {
        throw "Unable to determine the site-packages directory for $python"
    }
    $sitePackages = [IO.Path]::GetFullPath($sitePackages.Trim())

    $develRoot = if ($rocmPath) {
        [IO.Path]::GetFullPath($rocmPath)
    } else {
        Join-Path $sitePackages "_rocm_sdk_devel"
    }
    if ((Split-Path -Leaf $develRoot) -ne "_rocm_sdk_devel" -or
        -not (Test-SameWindowsPath -Left (Split-Path -Parent $develRoot) -Right $sitePackages)) {
        throw "ROCM_PATH does not name the development package in the selected virtual environment: $develRoot"
    }
    $coreRoot = Join-Path $sitePackages "_rocm_sdk_core"
    foreach ($path in @($develRoot, $coreRoot)) {
        if (-not (Test-Path $path -PathType Container)) {
            throw "Required ROCm SDK package directory was not found: $path"
        }
    }

    [pscustomobject]@{
        VenvRoot     = $VenvPath
        Python       = $python
        ScriptsRoot  = Join-Path $VenvPath "Scripts"
        SitePackages = $sitePackages
        CoreRoot     = $coreRoot
        DevelRoot    = $develRoot
        CoreBin      = Join-Path $coreRoot "bin"
        CoreLib      = Join-Path $coreRoot "lib"
        DevelBin     = Join-Path $develRoot "bin"
        DevelLib     = Join-Path $develRoot "lib"
        DevelInclude = Join-Path $develRoot "include"
    }
}

function Test-SameWindowsPath {
    param(
        [Parameter(Mandatory = $true)][string]$Left,
        [Parameter(Mandatory = $true)][string]$Right
    )

    return [IO.Path]::GetFullPath($Left).TrimEnd('\', '/').Equals(
        [IO.Path]::GetFullPath($Right).TrimEnd('\', '/'),
        [StringComparison]::OrdinalIgnoreCase)
}

function Get-WindowsDependencyRoot {
    param(
        [Parameter(Mandatory = $true)][string]$InstallPrefix,
        [Parameter(Mandatory = $true)][string]$ActiveDevelRoot
    )

    $installRoot = [IO.Path]::GetFullPath($InstallPrefix)
    if (Test-SameWindowsPath -Left $installRoot -Right $ActiveDevelRoot) {
        return [IO.Path]::GetFullPath($ActiveDevelRoot)
    }
    return $installRoot
}

function Remove-WindowsInstallEntry {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (Test-Path $Path -PathType Container) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    } elseif (Test-Path $Path) {
        Remove-Item -LiteralPath $Path -Force
    }
}

function Install-WindowsFile {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    if (-not (Test-Path $Source -PathType Leaf)) {
        throw "Source file does not exist: $Source"
    }
    if (Test-SameWindowsPath -Left $Source -Right $Destination) {
        Write-Host "Already installed $Destination"
        return
    }

    $destinationDirectory = Split-Path -Parent $Destination
    New-Item -ItemType Directory -Force -Path $destinationDirectory | Out-Null
    $temporary = Join-Path $destinationDirectory `
        (".{0}.{1}.installing" -f (Split-Path -Leaf $Destination), [guid]::NewGuid().ToString("N"))
    try {
        Copy-Item -LiteralPath $Source -Destination $temporary -Force
        Remove-WindowsInstallEntry -Path $Destination
        Move-Item -LiteralPath $temporary -Destination $Destination
    } finally {
        if (Test-Path $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
    Write-Host "Installed $Destination"
}

function Install-WindowsDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    if (-not (Test-Path $Source -PathType Container)) {
        throw "Source directory does not exist: $Source"
    }
    if (Test-SameWindowsPath -Left $Source -Right $Destination) {
        Write-Host "Already installed $Destination"
        return
    }

    $destinationParent = Split-Path -Parent $Destination
    New-Item -ItemType Directory -Force -Path $destinationParent | Out-Null
    $temporary = Join-Path $destinationParent `
        (".{0}.{1}.installing" -f (Split-Path -Leaf $Destination), [guid]::NewGuid().ToString("N"))
    try {
        Copy-Item -LiteralPath $Source -Destination $temporary -Recurse -Force
        Remove-WindowsInstallEntry -Path $Destination
        Move-Item -LiteralPath $temporary -Destination $Destination
    } finally {
        if (Test-Path $temporary) {
            Remove-Item -LiteralPath $temporary -Recurse -Force
        }
    }
    Write-Host "Installed $Destination"
}

function Find-NewestFile {
    param(
        [Parameter(Mandatory = $true)][string[]]$Roots,
        [Parameter(Mandatory = $true)][string]$FileName
    )

    $matches = foreach ($root in $Roots) {
        if (Test-Path $root) {
            Get-ChildItem -Path $root -Recurse -File -Filter $FileName -ErrorAction SilentlyContinue
        }
    }

    return $matches | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
}
