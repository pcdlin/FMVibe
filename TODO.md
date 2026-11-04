## New Features

- Remap modifiers for modulation sources
    - **In one line:** Random and MIDI (a controller or aftertouch, with Slew) sources came as card types; what is
      left is a modifier that bends another source's output through a drawn curve, or slews an LFO's or envelope's.
    - **Feasible, medium.** A source type whose input is another source (picked like an aux) and whose output is that
      source's through a curve laid out like the note source's; it runs after its input in the voice's source loop
      (`Synth::renderBlock()`), so an input in a later place is a block late. 7 spare ids a place are left.

- MODS view: the filter field over the table, and a search in the target menu (both in the draft)
    - **In one line:** a text field in the MODS view's title row narrowing the rows to those whose source and target
      names hold every word typed, and one atop the target menu listing the matching targets flat, with their path.
    - **Feasible, small:** the preset browser's search (`dialogs.cpp`) already edits text this way; the rows and the
      menu's entries are built by name (`modsRows()`, `cascadeTree()` in `mods.cpp`).

- Poly legato
    - **In one line:** with Legato on in poly, a note played while another key is down starts its envelopes at their
      sustain level instead of from the attack, so overlapping chords swell together instead of re-attacking.
    - **Feasible, small–medium.** Today Legato only acts in mono, where a new note takes over the sounding voice
      (`Synth::noteOn()` in `synth.cpp`); in poly every note gets a fresh voice, so there's nothing to keep running.
      To decide: whether the operators' envelopes and the mod envelopes both skip to sustain, and whether LFOs that
      restart with notes keep the held voice's phase. The Legato switch stops greying out in poly
      (`view.cpp`, the play switches), with a smoke check and features.md's Play modes.

## Issues 

## Improvements 

- Move factory presets out of the C++ code, into committed .FMV files. They should be compiled into the code on build.
    - **In one line:** move the preset reading and writing into the core library, export today's presets once as
      sparse `.fmv` files (only what differs from Init; a full one is ~128 KB), embed them with CMake as byte
      arrays, and drop the ~700-line builder in `factory.cpp`.
    - **Feasible, medium.**
        1. Move the patch format (`writePatch()` and the reading half of `loadState()`) from `plugin.cpp` into
           `fmvibe-core`, so the web demo, tests and tools can read a preset from memory.
        2. A small tool writes today's examples out once as `presets/Factory/*.fmv`, holding only the params that
           differ from Init (a full file is ~128 KB: 10,660 params × 12 bytes; missing params load as their
           defaults, which is how the C++ list treats them today), plus an order list, since the examples-given
           count in settings.txt depends on their order. Its `--normalize` makes an edited and saved preset sparse
           again.
        3. CMake reads the files (`file(READ … HEX)`) into a generated `.cpp` of byte arrays, for the plugin and the
           web build alike, regenerated when a file changes.
        4. `factoryPresets()` decodes those, and `writeExamples()` copies the bytes as they are; the builder in
           `factory.cpp` (~700 lines) goes. quality.cpp's preset checks and uishot keep using `factoryPresets()`.

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

- VST3 version?
    - **In one line:** use clap-wrapper, with four small fixes in FMVibe; per-voice host modulation, remote control
      pages and dynamically hidden params don't carry over to VST3.
    - **Feasible, medium (1–2 weeks, mostly host testing)**, through
      [clap-wrapper](https://github.com/free-audio/clap-wrapper), as AGENTS.md's Next steps already say. The VST3
      SDK has been MIT-licensed since 3.8. What follows was checked in clap-wrapper's source (main, October 2026).
        1. Fetch clap-wrapper and the VST3 SDK, pinned by hash like the CLAP SDK; `make_clapfirst_plugins()`
           builds `FMVibe.vst3` from the same code.
        2. The four fixes in FMVibe, which help the CLAP build too:
            - Add `clap.tail`. Without it the wrapper reports no tail, so hosts that suspend idle plugins could cut
              reverb and delay tails off.
            - Add the wrapper's `clap_plugin_as_vst3` extension, declaring the tuning, volume and pan note
              expressions (VST3 hosts get none otherwise) and a single MIDI channel. That cuts the parameters the
              wrapper adds for MIDI mapping from 2,080 (16 channels × 130) to 130.
            - Stop flagging FxOrder as stepped (it's hidden anyway). Its 16! − 1 steps overflow VST3's 32-bit step
              count (`detail/vst3/parameter.cpp:128` in the wrapper).
            - Map MIDI CC1 to the Mod Wheel param. The hardware wheel arrives as CC1, which FMVibe ignores today in
              both formats.
        3. Test raising the voice limit in each host: the restart becomes an I/O-changed restart, which most hosts
           answer by deactivating and reactivating the plugin.
        4. Run Steinberg's validator in `build.ps1 -Validate`, put the `.vst3` in the release zip, and try it by hand
           in VST3 hosts (Cubase, Ableton, FL, Reaper, Studio One).
    - What VST3 can't carry, wrapped or native: per-voice and global host modulation (in Bitwig, its per-note
      modulators and MPE through them; a VST3 gets one value per param for the whole plugin), `NOTE_END` and voice
      info, and remote control pages. Bitwig users keep the CLAP version.
    - **Native VST3 instead of the wrapper** (~4–6 weeks):
        - Pros:
            - Every automation point reaches the engine. The wrapper keeps only the last point of each param per
              block (`detail/vst3/process.cpp`), so fast automation and pitch bend move once a block. FMVibe ramps
              levels, sends and routing per sample, so this is mostly inaudible, but coarser at large buffers.
            - Control over what hosts see. The wrapper shows all 10,660 params (hidden ones flagged hidden) plus
              its MIDI-mapping ones. Natively, shape points, partials and wiring could stay in the state alone,
              FMVibe's own undo covering them and `IComponentHandler2::setDirty()` marking the project changed.
            - No dependence on the wrapper's quirks and releases (its 32-bit step count, its 20 ms main-thread
              timer on Windows).
        - Cons:
            - The work: split `plugin.cpp` into a host-neutral core and two front ends (~1 week), a VST3 front end
              of ~1,500–2,500 lines (~1.5–3 weeks), a VST3 test host and testing in hosts (~1–2 weeks).
            - Every host-facing change made twice from then on, and the CLAP smoke tests no longer cover what VST3
              hosts run.
            - No gain where VST3 itself falls short (above), nor in sound: it is identical either way, and the
              wrapper's own cost is microseconds a block.
        - Verdict: use the wrapper. If per-block automation ever proves audible, passing every point is a small
          patch to the wrapper (MIT) to send upstream.

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
