<#
.SYNOPSIS
Prepares a release: bumps the version, rebuilds and tests, refreshes docs\editor.png, then lists the commit, tag
and push and asks whether to do them (only "yes" does).

.EXAMPLE
.\release.ps1                # 0.6 -> 0.7: on "yes", commit "Version 0.7" on main, tag v0.7, push both
.\release.ps1 -Version 1.0   # a version of your choosing (major.minor)

Steps: check we are on main, with a clean tree, level with origin and the tag free; set the version in
CMakeLists.txt; build, run every test and both validators (build.ps1 -Validate, which also renders the editor's snapshots offscreen); copy the
snapshot of the PD Crystal Motion preset over docs\editor.png; ask; on "yes", commit both, tag vX.Y, and push main
and the tag. The tag makes GitHub Actions build the release and the browser demo (see AGENTS.md, "Releasing").

On anything but "yes", CMakeLists.txt and docs\editor.png go back as they were afterwards (so the script can run
again); the new screenshot stays at build\release\release-editor.png to look at.
#>
param(
    [ValidatePattern('^\d+\.\d+$')] [string] $Version
)
$ErrorActionPreference = 'Stop'

function Invoke-Git {
    & git @args
    if ($LASTEXITCODE) { throw "git $($args -join ' ') failed (exit code $LASTEXITCODE)" }
}

Push-Location $PSScriptRoot
try {
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

        $shot = "$PSScriptRoot\build\release\ui-shots\24-example-routing.png" # PD Crystal Motion, routing view
        if (-not (Test-Path $shot) -or (Get-Item $shot).LastWriteTime -lt (Get-Date).AddHours(-1)) {
            throw "The tests did not render $shot."
        }
        Copy-Item $shot "$PSScriptRoot\build\release\release-editor.png" -Force
        Copy-Item $shot "$PSScriptRoot\docs\editor.png" -Force

        Write-Host "== next (the new screenshot: build\release\release-editor.png):" -ForegroundColor Yellow
        Write-Host "   git add CMakeLists.txt docs/editor.png"
        Write-Host "   git commit -m `"Version $Version`""
        Write-Host "   git tag $tag"
        Write-Host "   git push origin main $tag"
        if ((Read-Host "Release $tag now? Type 'yes' to commit, tag and push") -ne 'yes') {
            Write-Host "== not released; CMakeLists.txt and docs\editor.png are put back as they were." -ForegroundColor Yellow
            return
        }

        # --- Commit, tag, push.
        Write-Host "== commit, tag $tag, push" -ForegroundColor Cyan
        Invoke-Git add CMakeLists.txt docs/editor.png
        Invoke-Git commit -m "Version $Version"
        Invoke-Git tag $tag
        Invoke-Git push origin main $tag
        Write-Host "== released $tag" -ForegroundColor Green
    }
    finally {
        # Nothing was committed (not confirmed, or a step failed): put the files back.
        if ((& git log -1 --format=%s) -ne "Version $Version") {
            & git checkout -- CMakeLists.txt docs/editor.png
        }
    }
}
finally {
    Pop-Location
}
