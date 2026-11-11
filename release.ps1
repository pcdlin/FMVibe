<#
.SYNOPSIS
Prepares a release: bumps the version, rebuilds and tests, refreshes the README's screenshots (docs\editor.png,
docs\fx-chain.png and docs\fx-graph.png), then lists the commit, tag and push and asks whether to do them (only "yes"
does).

.EXAMPLE
.\release.ps1                # 0.6 -> 0.7: on "yes", commit "Version 0.7" on main, tag v0.7, push both
.\release.ps1 -Version 1.0   # a version of your choosing (major.minor)
.\release.ps1 -Screenshots   # only render the README's screenshots into docs, on any branch, committing nothing

Steps: check we are on main, with a clean tree, level with origin and the tag free; set the version in
CMakeLists.txt; build, run every test and both validators (build.ps1 -Validate, which also renders the editor's snapshots offscreen); copy the
snapshots the tests render over the README's, in the Spectrum theme: the LD Screamer preset's editor over docs\editor.png, the
PD Crystal Motion preset's effects' panel as a chain over docs\fx-chain.png, and the BA Neon Pluck preset's effects' panel, a graph, over
docs\fx-graph.png; ask; on "yes", commit them all, tag vX.Y, and
push main and the tag. The tag makes GitHub Actions build the release and the browser demo (see AGENTS.md,
"Releasing").

On anything but "yes", CMakeLists.txt and the screenshots go back as they were afterwards (so the script can run
again); the new screenshots stay in build\release\ui-shots to look at.

-Screenshots does only the screenshots: it builds and runs fmvibe-uishot (the UI snapshots, with their checks) in the
release build and copies its renders over docs\editor.png, docs\fx-chain.png and docs\fx-graph.png, leaving the
version, the other tests and git alone.
#>
param(
    [ValidatePattern('^\d+\.\d+$')] [string] $Version,
    [switch] $Screenshots
)
$ErrorActionPreference = 'Stop'

function Invoke-Git {
    & git @args
    if ($LASTEXITCODE) { throw "git $($args -join ' ') failed (exit code $LASTEXITCODE)" }
}

# The README's screenshots, which the tests render in the Spectrum theme: the LD Screamer preset's routing view, the
# PD Crystal Motion preset's effects' panel as a chain, and the BA Neon Pluck preset's effects' panel, a graph split
# low and high.
function Copy-Screenshots {
    $shots = [ordered]@{
        'editor.png'   = '24c-screamer-example-routing.png'
        'fx-chain.png' = '24-example-fx-chain-panel.png'
        'fx-graph.png' = '24b-split-example-fx-graph-panel.png'
    }
    foreach ($doc in $shots.Keys) {
        $shot = "$PSScriptRoot\build\release\ui-shots\$($shots[$doc])"
        if (-not (Test-Path $shot) -or (Get-Item $shot).LastWriteTime -lt (Get-Date).AddHours(-1)) {
            throw "The tests did not render $shot."
        }
        Copy-Item $shot "$PSScriptRoot\docs\$doc" -Force
    }
}

Push-Location $PSScriptRoot
try {
    if ($Screenshots) {
        # cl, cmake and ninja come with Visual Studio: load its dev environment unless that already happened.
        if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
            $installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
            $vs = & "$installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if (-not $vs) { throw 'Visual Studio with the C++ build tools was not found.' }
            $env:PATH = "$installer;$env:PATH" # VsDevCmd calls vswhere by name
            Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
            Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
        }
        Write-Host '== build fmvibe-uishot' -ForegroundColor Cyan
        cmake --preset release
        if ($LASTEXITCODE) { throw "configure failed (exit code $LASTEXITCODE)" }
        cmake --build --preset release --target fmvibe-uishot
        if ($LASTEXITCODE) { throw "build failed (exit code $LASTEXITCODE)" }
        Write-Host '== render the snapshots' -ForegroundColor Cyan
        & "$PSScriptRoot\build\release\fmvibe-uishot.exe" "$PSScriptRoot\build\release\ui-shots"
        if ($LASTEXITCODE) { throw "fmvibe-uishot failed (exit code $LASTEXITCODE)" }
        Copy-Screenshots
        Write-Host '== rendered docs\editor.png, docs\fx-chain.png and docs\fx-graph.png (not committed)' -ForegroundColor Green
        return
    }

    # --- Checks first, before anything is touched.
    $branch = (& git branch --show-current).Trim()
    if ($branch -ne 'main') { throw "Releases are made from main; this is '$branch'." }
    if (& git status --porcelain) { throw 'The working tree has uncommitted changes; commit or stash them first.' }
    Invoke-Git fetch origin main --tags --quiet
    if ((& git rev-list --count 'HEAD..origin/main') -ne '0') { throw 'main is behind origin/main; pull first.' }

    # --- The version: CMakeLists.txt's project(), one minor step up unless -Version says otherwise.
    $cmake = Get-Content CMakeLists.txt -Raw
    $pattern = '(?m)^(project\(FMVibe VERSION )(\d+)\.(\d+)( LANGUAGES)'
    $m = [regex]::Match($cmake, $pattern)
    if (-not $m.Success) { throw 'Could not find the version in project() in CMakeLists.txt.' }
    $current = "$($m.Groups[2].Value).$($m.Groups[3].Value)"
    if (-not $Version) { $Version = "$($m.Groups[2].Value).$([int]$m.Groups[3].Value + 1)" }
    $tag = "v$Version"
    if (& git tag --list $tag) { throw "The tag $tag exists already." }
    Write-Host "== version $current -> $Version" -ForegroundColor Cyan

    try {
        $utf8 = New-Object System.Text.UTF8Encoding($false)
        [IO.File]::WriteAllText("$PSScriptRoot\CMakeLists.txt", [regex]::Replace($cmake, $pattern, "`${1}$Version`$4"), $utf8)

        # --- Build and test with the new version (the tests render the editor's snapshots as they go).
        & "$PSScriptRoot\build.ps1" -Validate
        if ($LASTEXITCODE) { throw "build.ps1 failed (exit code $LASTEXITCODE)" }

        Copy-Screenshots

        Write-Host "== next (the new screenshots: docs\editor.png, docs\fx-chain.png, docs\fx-graph.png):" -ForegroundColor Yellow
        Write-Host "   git add CMakeLists.txt docs/editor.png docs/fx-chain.png docs/fx-graph.png"
        Write-Host "   git commit -m `"Version $Version`""
        Write-Host "   git tag $tag"
        Write-Host "   git push origin main $tag"
        if ((Read-Host "Release $tag now? Type 'yes' to commit, tag and push") -ne 'yes') {
            Write-Host "== not released; CMakeLists.txt and the screenshots are put back as they were." -ForegroundColor Yellow
            return
        }

        # --- Commit, tag, push.
        Write-Host "== commit, tag $tag, push" -ForegroundColor Cyan
        Invoke-Git add CMakeLists.txt docs/editor.png docs/fx-chain.png docs/fx-graph.png
        Invoke-Git commit -m "Version $Version"
        Invoke-Git tag $tag
        Invoke-Git push origin main $tag
        Write-Host "== released $tag" -ForegroundColor Green
    }
    finally {
        # Nothing was committed (not confirmed, or a step failed): put the files back.
        if ((& git log -1 --format=%s) -ne "Version $Version") {
            & git checkout -- CMakeLists.txt docs/editor.png docs/fx-chain.png docs/fx-graph.png
        }
    }
}
finally {
    Pop-Location
}
