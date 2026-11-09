## New Features

- Amp effect: amp models (preamp, tone stack, power amp) with an optional cabinet emulation, like the guitar amp devices DAWs ship.
    - **Planned:** [docs/plans/amp/plan.md](docs/plans/amp/plan.md), with UI drafts beside it. Open questions at its
      end (draft A or B, the models and cabinets, oversampling, color).

- Make hard clip shaper stackable. Add a stack setting from 1 to 8 that behaves as if that shaper is repeated that many times in direct succession.

- Remap modifiers for modulation sources
    - **In one line:** Random and MIDI (a controller or aftertouch, with Slew) sources came as card types, and each
      modulation now has a curve of its own (an envelope segment's bend, in the MODS view); what is left is slewing
      an LFO's or envelope's output, and a freely drawn curve shared between modulations.
    - **Feasible, medium.** A source type whose input is another source (picked like an aux) and whose output is that
      source's slewed or through a drawn curve; it runs after its input in the voice's source loop
      (`Synth::renderBlock()`), so an input in a later place is a block late. 7 spare ids a place are left.


## Issues 

## Improvements 

## Architecture

- Linux support
    - **In one line:** a build without the editor first, then a Cairo drawing backend and an X11 editor window, and
      the tests ported (which also lets the UI snapshot checks run in Linux CI).
    - **Feasible, large.** The engine, `View` and the CLAP code are portable C++20. What's Windows-only is the
      window (`editor.cpp`), the drawing (`canvas.cpp`, Direct2D), a few calls in `plugin.cpp` (the Documents and
      %APPDATA% folders, `_wfopen_s`, `_wcsicmp`, `ShellExecuteW`), and the file dialogs, clipboard and keyboard
      hook.
        1. Headless first: the plugin and `fmvibe-quality` building on Linux (paths through `std::filesystem::path`;
           XDG folders: `~/.local/share/FMVibe/presets`, `~/.config/FMVibe`), without `clap.gui`, so hosts show
           their own param list.
        2. A third `Canvas` backend on Cairo with a bundled font, and a `CLAP_WINDOW_API_X11` editor window whose X
           events and redraw timer run on the host's `posix-fd-support` and `timer-support`.
        3. File dialogs through the XDG desktop portal (or zenity/kdialog), xdg-open for the folder, X11 selections
           for the clipboard.
        4. Port the smoke host (dlopen) and uishot (Cairo PNGs, so the UI checks can run in Linux CI too), and add a
           Linux job to release.yml.

## Organziational

- Decide on names for the major UI elements to put into documentation
    - **In one line:** add a glossary at the top of `docs/features.md`; most names are already used there.
    - **Feasible, small** (a decision; a draft is quick). features.md already names most of them; a glossary at
      its top would fix them: header, preset bar, view tabs, routing panel (graph, matrix), inspector, FX chain
      and cards, modulation panel and source cards, source handle, depth handle, modulation ring, keyboard strip
      (note source, macros, wheels, keys), the shape, partial and curve editors, and the settings dialog.
    - Decided: the docs say "modulation" (not "automation", which in a DAW means host automation) and "depth
      handle" (not "blip" or "dot"). The modulation list's tab is **MODS** for now, apart from MATRIX (the routing
      grid). features.md says "depth handle" in places now, but its Modulation section still introduces them as
      small dots.

## TBD

- Filter drive sounds strange, investigate
