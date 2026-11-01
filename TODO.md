## Bugs

## New Features

- Add auxiliary modulation sources, like Phase Plant and Serum
    - **Feasible.** Read as the aux-source mechanism the next item spells out, and planned there (its steps 1, 2
      and 4 are the engine and the drag; the mod matrix only shows it).
    - If it means more source *types* instead (velocity, aftertouch, random per note, a MIDI CC, slew or remap
      modifiers): each is a new `SourceType` appended to the list, its fields at the end of `SourceParam` (12 spare
      ids a place), a card body and a smoke check.

- Add an automation matrix that lists all active automation from LFOs, Env, Macros, Note, Modwheel and allows changing their parameters
    - Extra navigation entry next to Routing/Matrix/FX called "Mod matrix"
    - Also has an extra "amount" slider per automation that fine-controls the modulation amount
    - Also shows the aux source
    - Allow using a automation source (LFOs, Env, Macros, Note, Modwheel) as an "aux source" for an existing automation by dragging it to a control's automation blip
    - The aux source controls the amount of the original automation being applied
    - Only one aux source allowed per automation
    - **Feasible; the biggest item** (params, engine, a new view, tests). Plan:
        1. Params: a new section with two fields per slot of every source (16 places, 8 macros, note, mod wheel):
           *Aux* (wiring like a target: source + 1, 0 = none, not automatable) and *Amount* (0–100 %, 100 to
           begin with, automatable), at ids of their own (say 22000 + 40·source). One Aux field per slot makes
           the "only one aux" rule.
        2. Engine (`synth.cpp`, `collectSlots` and the macro, note and mod wheel paths): a slot's offset = depth ×
           Amount × the aux's value (0–1, taken as unipolar; the previous block's for per-voice sources, as sources
           already see each other a block late). Macro and mod wheel slots stay global unless their aux runs per
           voice; then that slot goes into the voice's slot mods.
        3. Editor: `RoutingView::ModMatrix` appended (it is stored by number), a tab after FX, and a table across
           both panels like FX: a row per modulation with its source (its color; a click shows its card), target,
           a wide depth bar, Amount, Aux (a menu or a dropped handle; × clears it), bypass and remove. It scrolls
           like the slot lists.
        4. Dropping a source's handle on a depth dot (ignored today, see `DragKind::ModSource`) sets that slot's
           Aux, replacing the one there; the dot gets a ring in the aux's color.
        5. Checks: smoke (aux at 0 silences the slot, Amount at 50 % halves it, a per-voice aux on a macro slot);
           uishot replays for the tab, a drop on a dot and a row's controls; features.md.
    - Open: is "amount" a 0–100 % scaler on the depth (Serum's *Output*, as planned), or how far the aux scales
      the depth? And "MATRIX" and "MOD MATRIX" side by side read alike (see Organizational).

- Toggleable pitch tracking for operators, fixed frequncy if turned off
    - **Feasible, small.**
        1. Two fields at the end of `OpParam`, after the shaper sends, so no id moves (25 of 50 ids used): *Key
           Track* (on to begin with) and *Freq* (1 Hz–20 kHz on a log curve, 440 Hz to begin with).
        2. `synth.cpp`: off, the operator's increment comes from Freq instead of the note's Hz × ratio, so the
           note, bend, glide and global Pitch don't move it; its Pitch, Detune and unison detune still do.
        3. Editor: a switch beside the ratio knob; off, a Freq knob takes Coarse's place, and the graph node and
           summary line show Hz instead of the ratio.
        4. Smoke: a fixed operator's peak stays put across two notes; features.md.

## Issues 

- When assigning too many automation sources to a control, the automation blips spill over onto other elements. Not sure how to solve this.
    - **Feasible.** The dots under a knob are laid 9 px apart with no limit, and those on labels and cells 6–7 px
      apart leftwards (`view.cpp`, "Depth handles in the gap under each modulated knob").
        1. Show as many as fit the knob's (label's, cell's) width; past that, the last place becomes a "+n" pill.
        2. Hovering or clicking the pill lists the rest in a small popup, each with its dot (drag, bypass,
           remove); once the mod matrix exists, it can open that instead, filtered to the target.
        3. uishot: eight sources on one knob, nothing drawn outside its column (read off the pixels).


## Improvements 

- Move factory presets out of the C++ code, into committed .FMV files. They should be compiled into the code on build.
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

- In the LFO card, the rate knob area is a bit cramped
    - **Feasible, small.** The Rate knob (30 px) has its top against the title row, above the shape buttons, 19 px
      from the last of them, with the sync switch right under its label.
        1. Narrow the shape buttons' pitch from 31 to 28 px and the three mode switches to match (~18 px freed).
        2. Centre the knob in the wider column at the buttons' height, the sync switch under it, taking the few px
           it needs from the display (52 → 46 px high).
        3. Check the uishot shots at scale 2, synced (a label like 1/16T) and free.

- The shimmer reverb does not sound good
    - **Feasible, but needs ears**: this goes by WAV renders compared by listening. Likely causes (`effects.cpp`):
      the shifting happens inside four of the network's lines, with two read heads over a fixed 80 ms window,
      all moving at the same rate an eighth of a cycle apart. An octave up, that is a 12.5 Hz crossfade flutter,
      applied again on every pass, with nothing after it to smear the grains.
        1. Take the shifter out of the tank: the tank's output → a pitch shifter per side (three or four
           Hann-windowed heads over a longer window, 100–150 ms, each grain's length jittered so the flutter doesn't
           repeat) → the band-pass → a few all-passes → back into the tank's input, by Shimmer, kept under unity.
        2. A small tool renders WAVs (a chord into the hall and the shimmer at a few settings) to compare the old
           and the new.
        3. Retune the quality and smoke checks (the octaves climbing, the octave about as loud as the note in the
           second after it ends) and PD Shimmer Pad's level.

## Architecture

- Linux support
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
    - **Feasible, medium**, through [clap-wrapper](https://github.com/free-audio/clap-wrapper), as AGENTS.md's
      Next steps already say. The VST3 SDK has been MIT-licensed since 3.8.
        1. Fetch clap-wrapper and the VST3 SDK, pinned by hash like the CLAP SDK; `make_clapfirst_plugins()`
           builds `FMVibe.vst3` from the same code.
        2. What VST3 lacks falls back: per-voice host modulation and `NOTE_END`, remote control pages, hiding the
           other type's fields (all ~10,700 params show, so mark the wiring ones hidden); the voice limit's restart
           becomes a component restart. Check how the wrapper maps each.
        3. Run Steinberg's validator in `build.ps1 -Validate`, put the `.vst3` in the release zip, and try it by hand
           in a VST3 host.

## Organziational

- Decide on names for the major UI elements to put into documentation
    - **Feasible, small** (a decision; a draft is quick). features.md already names most of them; a glossary at
      its top would fix them: header, preset bar, view tabs, routing panel (graph, matrix), inspector, FX chain
      and cards, modulation panel and source cards, source handle, depth dot, modulation ring, keyboard strip
      (note source, macros, wheels, keys), the shape, partial and curve editors, and the settings dialog.
    - To decide: this file says "automation" and "blip" where the docs say "modulation" and "depth handle" or
      "dot". In a DAW, "automation" means host automation, so "modulation" is clearer. A "MOD MATRIX" tab beside
      "MATRIX" (the routing grid) needs a name that tells them apart, such as "MODS".
