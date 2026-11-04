## New Features

- Remap modifiers for modulation sources
    - **In one line:** Random and MIDI (a controller or aftertouch, with Slew) sources came as card types, and each
      modulation now has a curve of its own (an envelope segment's bend, in the MODS view); what is left is slewing
      an LFO's or envelope's output, and a freely drawn curve shared between modulations.
    - **Feasible, medium.** A source type whose input is another source (picked like an aux) and whose output is that
      source's slewed or through a drawn curve; it runs after its input in the voice's source loop
      (`Synth::renderBlock()`), so an input in a later place is a block late. 7 spare ids a place are left.


## Issues 

## Improvements 

- The shimmer reverb does not sound good
    - **In one line:** move the pitch shifter out of the reverb's delay lines into a feedback path after it, with
      jittered windows and diffusion after it, judged by listening to WAV renders.
    - **Feasible, medium, but needs ears**: this goes by WAV renders compared by listening. Likely causes
      (`effects.cpp`): the shifting happens inside four of the network's lines, with two read heads over a fixed
      80 ms window, all moving at the same rate an eighth of a cycle apart. An octave up, that is a 12.5 Hz
      crossfade flutter, applied again on every pass, with nothing after it to smear the grains.
        1. Take the shifter out of the tank: the tank's output → a pitch shifter per side (three or four
           Hann-windowed heads over a longer window, 100–150 ms, each grain's length jittered so the flutter doesn't
           repeat) → the band-pass → a few all-passes → back into the tank's input, by Shimmer, kept under unity.
        2. A small tool renders WAVs (a chord into the hall and the shimmer at a few settings) to compare the old
           and the new.
        3. Retune the quality and smoke checks (the octaves climbing, the octave about as loud as the note in the
           second after it ends) and PD Shimmer Pad's level.

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
