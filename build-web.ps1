<#
.SYNOPSIS
Builds FMVibe's browser demo with Emscripten into build\web\site, and serves it.

.EXAMPLE
.\build-web.ps1                 # build -> build\web\site
.\build-web.ps1 -Serve          # build, then serve it on http://localhost:8080
.\build-web.ps1 -Serve -Port 9000
#>
param(
    [switch] $Serve,
    [int] $Port = 8080
)
$ErrorActionPreference = 'Stop'

# Emscripten, pinned like the other fetched dependencies. It lives in build\emsdk (about 1 GB installed).
$EmsdkVersion = '6.0.13'

# cmake and ninja come with Visual Studio; load its dev environment into this session unless that already happened.
if (-not (Get-Command ninja.exe -ErrorAction SilentlyContinue)) {
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
    $emsdk = Join-Path $PSScriptRoot 'build\emsdk'
    if (-not (Test-Path "$emsdk\emsdk.bat")) {
        Invoke-Step 'fetch emsdk' { git clone --quiet https://github.com/emscripten-core/emsdk $emsdk }
    }
    $installed = Join-Path $emsdk '.fmvibe-version'
    if ((Get-Content $installed -ErrorAction SilentlyContinue) -ne $EmsdkVersion) {
        Invoke-Step "emsdk $EmsdkVersion" { git -C $emsdk fetch --quiet --tags; git -C $emsdk -c advice.detachedHead=false checkout --quiet $EmsdkVersion }
        Invoke-Step "install Emscripten $EmsdkVersion" { & "$emsdk\emsdk.bat" install $EmsdkVersion }
        Invoke-Step "activate Emscripten $EmsdkVersion" { & "$emsdk\emsdk.bat" activate $EmsdkVersion }
        Set-Content $installed $EmsdkVersion
    }
    . "$emsdk\emsdk_env.ps1" *> $null # EMSDK, emcc and its Python into this session

    Invoke-Step 'configure' { cmake --preset web }
    Invoke-Step 'build' { cmake --build --preset web }
    Write-Host "The demo is in build\web\site." -ForegroundColor Green

    if ($Serve) {
        $python = if ($env:EMSDK_PYTHON) { $env:EMSDK_PYTHON } else { 'python' }
        & $python web\serve.py build\web\site $Port
    }
}
finally {
    Pop-Location
}
