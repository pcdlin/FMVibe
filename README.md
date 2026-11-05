# FMVibe

A six-operator FM synthesizer as a [CLAP](https://github.com/free-audio/clap) and a
[VST3](https://github.com/steinbergmedia/vst3sdk) instrument plugin for Windows. It works in the spirit of other
popular FM synths. Both are built from the same code; the VST3 one is written straight against the VST3 interfaces,
without a wrapper.

In-browser demo: [https://pcdlin.github.io/FMVibe/](https://pcdlin.github.io/FMVibe/)

![FMVibe UI](docs/editor.png)

Implemented using Claude Code.

The full feature reference is in [docs/features.md](docs/features.md); working on the code, its tests and the
plugin's internals are in [AGENTS.md](AGENTS.md).

## Download

Each [release](https://github.com/pcdlin/FMVibe/releases) has a zip for 64-bit Windows with `FMVibe.clap` and the
`FMVibe.vst3` folder; use whichever your DAW takes, or both.

- Put `FMVibe.clap` into either the per-user CLAP folder, `%LOCALAPPDATA%\Programs\Common\CLAP`, or the system-wide
  one, `%COMMONPROGRAMFILES%\CLAP`.
- Put the whole `FMVibe.vst3` folder into either the per-user VST3 folder, `%LOCALAPPDATA%\Programs\Common\VST3`,
  or the system-wide one, `%COMMONPROGRAMFILES%\VST3`. Not every DAW looks in the per-user one; if yours doesn't,
  use the system-wide one or add the folder to the DAW's plugin folders.

Copying into a system-wide folder needs administrator rights.

## Build

You need:

- Visual Studio 2026 with the *Desktop development with C++* workload. That workload already includes CMake and
  Ninja, so nothing else needs installing.
- Internet access on the first configure. CMake fetches the CLAP SDK 1.2.10, the VST3 SDK 3.8.1 (its interfaces,
  and the parts its validator is built from) and clap-validator 0.4.1, all pinned by SHA-256, into
  `build/<config>/_deps`.

```powershell
.\build.ps1                    # release: configure, build, tests
.\build.ps1 -Config debug      # debug build in build\debug
.\build.ps1 -Validate          # also run clap-validator and Steinberg's VST3 validator against the plugins
.\build.ps1 -Install           # also copy the plugins to %LOCALAPPDATA%\Programs\Common\CLAP and ...\VST3
.\build.ps1 -Install -SkipTests  # build only the plugins and install them, without the tests
.\build.ps1 -Install -Format vst3 -SkipTests  # the same for the VST3 plugin alone (or -Format clap)
```

The plugins end up at `build\<config>\FMVibe.clap` and `build\<config>\FMVibe.vst3` (a folder, as VST3 has it).
To try the plugin without a DAW, run `build\<config>\fmvibe-uihost.exe`: it opens the editor in a window and plays
it through the default output device, with notes from the editor's keyboard and any MIDI input.
`build.ps1` loads the Visual Studio developer environment itself, so it works from any PowerShell window. Once it
has run in a session, the plain CMake commands work too:

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release
cmake --build --preset release --target validate
cmake --install build\release
```

You can also open the folder directly in Visual Studio, or in VS Code with the CMake Tools extension: both pick up
the `debug` and `release` presets from `CMakePresets.json`.

`.\build.ps1 -Install` copies the plugins into the per-user CLAP and VST3 folders
(`%LOCALAPPDATA%\Programs\Common\CLAP` and `%LOCALAPPDATA%\Programs\Common\VST3`), where hosts such as DAWs find
them.

## License

FMVibe is released under the [MIT license](LICENSE).
