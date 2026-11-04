<#
.SYNOPSIS
Configures, builds and tests FMVibe, the CLAP and the VST3 plugin, with the Visual Studio C++ toolchain.

.EXAMPLE
.\build.ps1                     # release build + tests -> build\release\FMVibe.clap and build\release\FMVibe.vst3
.\build.ps1 -Config debug
.\build.ps1 -Validate           # also run clap-validator and Steinberg's VST3 validator on the plugins
.\build.ps1 -Install            # also copy them to %LOCALAPPDATA%\Programs\Common\CLAP and ...\Common\VST3
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
    # Without the tests, only the plugins themselves are built.
    $targets = if ($SkipTests) { @('--target', 'FMVibe', 'FMVibe-vst3') } else { @() }
    Invoke-Step 'build' { cmake --build --preset $Config @targets }
    if (-not $SkipTests) { Invoke-Step 'tests' { ctest --preset $Config } }
    if ($Validate) { Invoke-Step 'validators' { cmake --build --preset $Config --target validate } }
    if ($Install) { Invoke-Step 'install' { cmake --install "build\$Config" } }
}
finally {
    Pop-Location
}
