<#
.SYNOPSIS
Compares builds of the CLAP plugin: whether they play the same and what each costs.

.DESCRIPTION
Builds the CLAP plugin at each commit given (or with the working tree's changes) in a worktree of its own under
build\bench, keeps each build there, and runs fmvibe-bench (tools/bench.cpp) on them:

- render: every factory preset through each build, as saved and at 8 unison copies, compared bit for bit with the
  first build's;
- time: each load (16 notes on the default patch; the 6-operator chain at 1 and at 8 unison copies; nothing playing;
  the chain decaying and releasing; and the 1-copy chain with 8 host events a block, changing nothing, automating and
  modulating params) in ms of CPU per second of audio, the builds taking turns round after round on
  one logical processor, so a machine that gets busier for a while weighs on all of them alike. Each figure is the
  median of the rounds, each round the best of -Passes seconds.

Timing wants a quiet machine: the load is printed, and a busy one warned about. Compare a change against the commit
before it, with one copy and with eight, before telling what it costs: MSVC's code for the voices' loop moves with
changes that don't touch it.

.EXAMPLE
.\tools\bench.ps1 -Commits HEAD~1,HEAD           # a commit against the one before
.\tools\bench.ps1 -Commits HEAD,Working          # the working tree's changes, new files too, against HEAD
.\tools\bench.ps1 -Commits v0.7,v0.8,HEAD -SkipRender
.\tools\bench.ps1 -Plugins a.clap,b.clap         # plugins built already
#>
param(
    [string[]] $Commits = @(),
    [string[]] $Plugins = @(),
    [int] $Rounds = 5,
    [int] $Passes = 5,
    [int] $Cpu = 2, # a logical processor on a performance core
    [switch] $SkipRender,
    [switch] $SkipTime
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'build\bench'
New-Item -ItemType Directory -Force $out | Out-Null

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { # as build.ps1 does
    $installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
    $vs = & "$installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio with the C++ build tools was not found.' }
    $env:PATH = "$installer;$env:PATH"
    Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}

function Invoke-Native([string] $What, [string] $Log, [scriptblock] $Command) {
    & $Command *>> $Log
    if ($LASTEXITCODE) { throw "$What failed (exit code $LASTEXITCODE); see $Log" }
}

# fmvibe-bench itself, from this checkout's release build.
$bench = Join-Path $root 'build\release\fmvibe-bench.exe'
if (-not (Test-Path $bench) -or (Get-Item $bench).LastWriteTime -lt (Get-Item (Join-Path $root 'tools\bench.cpp')).LastWriteTime) {
    Write-Host '== building fmvibe-bench' -ForegroundColor Cyan
    Push-Location $root
    try {
        $log = Join-Path $out 'fmvibe-bench.log'
        Remove-Item $log -ErrorAction SilentlyContinue
        if (-not (Test-Path 'build\release\build.ninja')) { Invoke-Native 'configure' $log { cmake --preset release } }
        Invoke-Native 'building fmvibe-bench' $log { cmake --build --preset release --target fmvibe-bench }
    } finally { Pop-Location }
}

# Each commit's plugin, built in a worktree kept for the next time (its builds are incremental), at a short path.
$builds = @()
if ($Commits.Count) {
    $worktree = Join-Path $out 'worktree'
    if (-not (Test-Path (Join-Path $worktree '.git'))) {
        git -C $root worktree add --quiet --detach $worktree HEAD
        if ($LASTEXITCODE) { throw 'git worktree add failed' }
    }
    $index = 0
    foreach ($commit in $Commits) {
        ++$index
        $working = $commit -ieq 'Working'
        $sha = git -C $root rev-parse --verify --quiet "$(if ($working) { 'HEAD' } else { $commit })^{commit}"
        if (-not $sha) { throw "no such commit: $commit" }
        $label = if ($working) { 'working' } else { git -C $root rev-parse --short $sha }
        Write-Host "== building $commit ($label)" -ForegroundColor Cyan
        git -C $worktree checkout --quiet --force --detach $sha
        git -C $worktree clean --quiet -fd
        if ($working) {
            # Every change against HEAD, new files too (those .gitignore doesn't leave out), through an index of its
            # own, so the checkout's own index, and what is staged there, stays as it is.
            $patch = Join-Path $out 'working.diff'
            $tempIndex = Join-Path $out 'working.index'
            Remove-Item $tempIndex -ErrorAction SilentlyContinue
            $env:GIT_INDEX_FILE = $tempIndex
            try {
                git -C $root read-tree HEAD
                git -C $root add --all
                git -C $root diff --cached HEAD --binary --output=$patch
            } finally {
                Remove-Item Env:GIT_INDEX_FILE
                Remove-Item $tempIndex -ErrorAction SilentlyContinue
            }
            if ((Get-Item $patch).Length -eq 0) { Write-Warning 'the working tree has no changes' }
            else { git -C $worktree apply --whitespace=nowarn $patch; if ($LASTEXITCODE) { throw 'applying the working tree changes failed' } }
        }
        $log = Join-Path $out "$index-$label.log"
        Remove-Item $log -ErrorAction SilentlyContinue
        Push-Location $worktree
        try {
            Invoke-Native 'configure' $log { cmake --preset release }
            Invoke-Native "building $commit" $log { cmake --build --preset release --target FMVibe }
        } finally { Pop-Location }
        $path = Join-Path $out "$index-$label.clap"
        Copy-Item (Join-Path $worktree 'build\release\FMVibe.clap') $path -Force
        $builds += [pscustomobject]@{ Label = "$index-$label"; Path = $path }
    }
    git -C $worktree checkout --quiet --force --detach HEAD # leaves no patch behind, nor the new files it brought
    git -C $worktree clean --quiet -fd
}
foreach ($plugin in $Plugins) {
    $path = (Resolve-Path $plugin).Path
    $builds += [pscustomobject]@{ Label = [IO.Path]::GetFileNameWithoutExtension($path); Path = $path }
}
if (-not $builds.Count) { throw 'nothing to compare: give -Commits or -Plugins' }

# The plugins read the user's settings as they start: these runs read none of theirs.
$env:FMVIBE_SETTINGS_DIR = Join-Path $out 'settings'

if (-not $SkipRender) {
    Write-Host '== render: every factory preset, as saved and at 8 unison copies' -ForegroundColor Cyan
    $presets = Join-Path $root 'presets\Factory'
    $reference = $null
    foreach ($b in $builds) {
        $played = [ordered]@{}
        foreach ($line in (& $bench render $b.Path $presets)) {
            if ($line -match '^([0-9a-f]{16}) ([0-9a-f]{16}) [0-9.]+ (.*)$') { $played[$Matches[3]] = "$($Matches[1]) $($Matches[2])" }
        }
        if ($LASTEXITCODE -or -not $played.Count) { throw "fmvibe-bench render failed on $($b.Path)" }
        $unloaded = @($played.Keys | Where-Object { $_ -like '(not loaded)*' }).Count
        if ($unloaded) { Write-Warning "$($b.Label): $unloaded presets didn't load (made by a later version?)" }
        if (-not $reference) {
            $reference = @{ Label = $b.Label; Played = $played }
            "{0}: {1} presets rendered" -f $b.Label, $played.Count
            continue
        }
        $differ = @($played.Keys | Where-Object { $reference.Played[$_] -ne $played[$_] })
        $differ += @($reference.Played.Keys | Where-Object { -not $played.Contains($_) })
        if ($differ.Count) { "{0}: DIFFERS from {1} on {2} of {3} presets: {4}" -f $b.Label, $reference.Label, $differ.Count, $reference.Played.Count, ($differ -join ', ') }
        else { "{0}: plays the same as {1}, bit for bit" -f $b.Label, $reference.Label }
    }
}

if (-not $SkipTime) {
    Write-Host "== time: $Rounds rounds, each the best of $Passes s, on logical processor $Cpu" -ForegroundColor Cyan
    $names = 'light', 'one', 'eight', 'idle', 'decay', 'release'
    $runs = @{}
    foreach ($b in $builds) { $runs[$b.Label] = @() }
    $loads = @()
    foreach ($round in 1..$Rounds) {
        foreach ($b in $builds) {
            try { $loads += (Get-Counter '\Processor(_Total)\% Processor Time' -SampleInterval 1 -MaxSamples 1).CounterSamples[0].CookedValue } catch {}
            $line = & $bench time $b.Path $Passes $Cpu
            if ($line -notmatch '^light ([0-9.]+) one ([0-9.]+) eight ([0-9.]+) idle ([0-9.]+) decay ([0-9.]+) release ([0-9.]+)') {
                throw "fmvibe-bench time failed on $($b.Path)"
            }
            $figures = @(1..6 | ForEach-Object { [double]$Matches[$_] })
            if ($line -match 'still ([0-9.]+) split ([0-9.]+) automated ([0-9.]+) modulated ([0-9.]+)') {
                $figures += @(1..4 | ForEach-Object { [double]$Matches[$_] })
            }
            $runs[$b.Label] += , $figures
        }
    }
    function Median($values) { $sorted = @($values | Sort-Object); $sorted[[int][Math]::Floor(($sorted.Count - 1) / 2)] }
    if ($loads.Count) {
        $load = Median $loads
        "machine load before each run: median {0:N1} %, most {1:N1} %" -f $load, ($loads | Measure-Object -Maximum).Maximum
        if ($load -gt 15) { Write-Warning 'the machine was busy: the figures are less sure' }
    }
    'ms of CPU per second of audio (16 voices at 48 kHz, 2x oversampling), median of the rounds; change against the first'
    "{0,-14} {1,9} {2,-17} {3,-17} {4,8} {5,-17} {6,-17}" -f 'build', 'light', '   1 copy', ' 8 copies', 'idle', ' decaying', 'releasing'
    $base = $null
    foreach ($b in $builds) {
        $median = @(0..5 | ForEach-Object { $k = $_; Median ($runs[$b.Label] | ForEach-Object { $_[$k] }) })
        $first = -not $base
        if ($first) { $base = $median }
        # A figure, and after the first build's its change against that one's.
        $cell = {
            param($k)
            $change = if ($first) { '' } else { '{0:+0.0;-0.0}%' -f (100.0 * ($median[$k] / $base[$k] - 1.0)) }
            '{0,9:N1} {1,7}' -f $median[$k], $change
        }
        "{0,-14} {1,9:N1} {2} {3} {4,8:N2} {5} {6}" -f $b.Label, $median[0], (& $cell 1), (& $cell 2), $median[3], (& $cell 4), (& $cell 5)
    }
    # Host automation and modulation on the 1-copy load, 8 events a block (see tools/bench.cpp).
    if (@($builds | Where-Object { $runs[$_.Label][0].Count -ge 10 }).Count -eq $builds.Count) {
        ''
        '1 copy with 8 host events a block: none, unchanged values, automation, modulation'
        "{0,-14} {1,-17} {2,-17} {3,-17} {4,-17}" -f 'build', '    still', '    split', ' automated', ' modulated'
        $base = $null
        foreach ($b in $builds) {
            $median = @(6..9 | ForEach-Object { $k = $_; Median ($runs[$b.Label] | ForEach-Object { $_[$k] }) })
            $first = -not $base
            if ($first) { $base = $median }
            $cell = {
                param($k)
                $change = if ($first) { '' } else { '{0:+0.0;-0.0}%' -f (100.0 * ($median[$k] / $base[$k] - 1.0)) }
                '{0,9:N1} {1,7}' -f $median[$k], $change
            }
            "{0,-14} {1} {2} {3} {4}" -f $b.Label, (& $cell 0), (& $cell 1), (& $cell 2), (& $cell 3)
        }
    }
}
