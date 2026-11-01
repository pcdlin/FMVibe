<#
.SYNOPSIS
Configures, builds and smoke-tests FMVibe with the Visual Studio C++ toolchain.

.EXAMPLE
.\build.ps1                     # release build + smoke test -> build\release\FMVibe.clap
.\build.ps1 -Config debug
.\build.ps1 -Validate           # also run clap-validator on the plugin
.\build.ps1 -Install            # also copy it to %LOCALAPPDATA%\Programs\Common\CLAP
.\build.ps1 -Install -SkipTests # build and install without running the tests
#>
param(
    [ValidateSet('debug', 'release')] [string] $Config = 'release',
    [switch] $Validate,
    [switch] $Install,
    [switch] $SkipTests
)
$ErrorActionPreference = 'Stop'

# cl, cmake and ninja all come with Visual Studio; load its dev environment
# into this session unless that already happened.
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
    $vs = & "$installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio with the C++ build tools was not found.' }
    $env:PATH = "$installer;$env:PATH" # VsDevCmd calls vswhere by name
    Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}

function Invoke-Step([string] $What, [scriptblock] $Command) {
    Write-Host "== $What" -ForegroundColor Cyan
    & $Command
    if ($LASTEXITCODE) { throw "$What failed (exit code $LASTEXITCODE)" }
}

Push-Location $PSScriptRoot
try {
    Invoke-Step 'configure' { cmake --preset $Config }
    # Without the tests, only the plugin itself is built.
    $targets = if ($SkipTests) { @('--target', 'FMVibe') } else { @() }
    Invoke-Step 'build' { cmake --build --preset $Config @targets }
    if (-not $SkipTests) { Invoke-Step 'smoke test' { ctest --preset $Config } }
    if ($Validate) { Invoke-Step 'clap-validator' { cmake --build --preset $Config --target validate } }
    if ($Install) { Invoke-Step 'install' { cmake --install "build\$Config" } }
}
finally {
    Pop-Location
}
