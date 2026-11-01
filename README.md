# FMVibe

[https://pcdlin.github.io/FMVibe/](https://pcdlin.github.io/FMVibe/)

A six-operator FM synthesizer as a [CLAP](https://github.com/free-audio/clap) instrument plugin for Windows. It
works in the spirit of other popular FM synths.

Implemented using Claude Code.

The full feature reference is in [docs/features.md](docs/features.md); working on the code, its tests and the
plugin's internals are in [AGENTS.md](AGENTS.md).

## Download

Each [release](https://github.com/pcdlin/FMVibe/releases) has a zip with `FMVibe.clap` for 64-bit Windows. Put
`FMVibe.clap` into the per-user CLAP folder, `%LOCALAPPDATA%\Programs\Common\CLAP`, where hosts such as Bitwig
Studio find it.

## Build

You need:

- Visual Studio 2026 with the *Desktop development with C++* workload. That workload already includes CMake and
  Ninja, so nothing else needs installing.
- Internet access on the first configure. CMake fetches the CLAP SDK 1.2.10 and clap-validator 0.4.1, both
  pinned by SHA-256, into `build/<config>/_deps`.

```powershell
.\build.ps1                    # release: configure, build, tests
.\build.ps1 -Config debug      # debug build in build\debug
.\build.ps1 -Validate          # also run clap-validator against the plugin
.\build.ps1 -Install           # also copy the plugin to %LOCALAPPDATA%\Programs\Common\CLAP
.\build.ps1 -Install -SkipTests  # build only the plugin and install it, without the tests
```

The plugin ends up at `build\<config>\FMVibe.clap`. `build.ps1` loads the Visual Studio developer environment
itself, so it works from any PowerShell window. Once it has run in a session, the plain CMake commands work too:

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release
cmake --build --preset release --target validate
cmake --install build\release
```

You can also open the folder directly in Visual Studio, or in VS Code with the CMake Tools extension: both pick up
the `debug` and `release` presets from `CMakePresets.json`.

## Install in a host

`.\build.ps1 -Install` copies the plugin into the per-user CLAP folder (`%LOCALAPPDATA%\Programs\Common\CLAP`),
where hosts such as Bitwig Studio find it.

## License

FMVibe is released under the [MIT license](LICENSE).
