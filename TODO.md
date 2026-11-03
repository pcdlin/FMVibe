## New Features

- Add auxiliary modulation sources, like Phase Plant and Serum
    - **In one line:** the aux-source idea the next item spells out, so it's planned there; new source *types*
      instead would each be a new `SourceType`, with 12 spare ids a place.
    - **Feasible.** Read as the aux-source mechanism the next item spells out, and planned there (its steps 1, 2
      and 4 are the engine and the drag; the MODS view only shows it).
    - If it means more source *types* instead (aftertouch, random per note, a MIDI CC, slew or remap modifiers):
      each is a new `SourceType` appended to the list, its fields at the end of `SourceParam` (12 spare ids a place),
      a card body and a smoke check. A per-note one like velocity (done: a strip source with a curve, see
      `VelocityParam`) can go into the keyboard strip instead, as a source after `kVelocitySource`; its slots'
      polarities then take the next block of the polarity section.

- Add an automation matrix that lists all active automation from LFOs, Env, Macros, Note, Modwheel, Velocity and
  allows changing their parameters
    - Extra navigation entry next to Routing/Matrix/FX called "Mod matrix"
    - Also has an extra "amount" slider per automation that fine-controls the modulation amount
    - Also shows the aux source
    - Allow using a automation source (LFOs, Env, Macros, Note, Modwheel) as an "aux source" for an existing automation by dragging it to a control's automation blip
    - The aux source controls the amount of the original automation being applied
    - Only one aux source allowed per automation
    - **In one line:** two new fields on every slot of every source, Aux (which source) and Amount (0–100 %); the
      engine multiplies depth × Amount × the aux's value; a **MODS** tab after FX lists every modulation as a row;
      dropping a source's handle on a depth handle (which does nothing today) sets that slot's aux.
    - **Feasible; the biggest item** (params, engine, a new view, tests). Plan:
        1. Params: a new section with two fields per slot of every source (16 places, 8 macros, note, mod wheel,
           velocity):
           *Aux* (wiring like a target: source + 1, 0 = none, not automatable) and *Amount* (a 0–100 % scaler on
           the depth, like Serum's Output; 100 to begin with, automatable), at ids of their own (say 22000 +
           40·source, clear of the slots' polarities at 24000 + 20·source). One Aux field per slot makes the "only one
           aux" rule.
        2. Engine (`synth.cpp`, `collectSlots` and the macro, note, velocity and mod wheel paths): a slot's offset =
           depth × Amount × the aux's value (0–1, taken as unipolar; the previous block's for per-voice sources, as
           sources already see each other a block late). Macro and mod wheel slots stay global unless their aux runs per
           voice; then that slot goes into the voice's slot mods.
        3. Editor: `RoutingView::Mods` appended (it is stored by number), a **MODS** tab after FX, and a table across
           both panels like FX: a row per modulation with its source (its color), target, a wide depth bar, its
           polarity switch (as on a source's row), Amount, Aux (a menu or a dropped handle; × clears it), bypass and
           remove. It scrolls like the slot lists.
           Clicking a row's source or target picks another from a menu, which moves the modulation there: it keeps
           its depth, polarity, Amount and Aux (and its bypass), as one undo step. The source moves into a free slot of
           the new source (refused, as a drop is, when that source already modulates the target or has no free slot);
           the target changes in place.
        4. Dropping a source's handle on a depth handle (ignored today, see `DragKind::ModSource`) sets that slot's
           Aux, replacing the one there; the depth handle gets a ring in the aux's color.
        5. Checks: smoke (aux at 0 silences the slot, Amount at 50 % halves it, a per-voice aux on a macro slot);
           uishot replays for the tab, a drop on a depth handle and a row's controls; features.md.
    - Decided: Amount is a 0–100 % scaler on the depth, like Serum's Output. The tab is called **MODS** for now.



## Issues 

- When assigning too many automation sources to a control, the automation blips spill over onto other elements. Not sure how to solve this.
    - **In one line:** show only as many depth handles as fit under the knob, label or cell, plus a "+n" pill that
      opens a list of the rest.
    - **Feasible, small.** The depth handles under a knob are laid 9 px apart with no limit, and those on labels
      and cells 6–7 px apart leftwards (`view.cpp`, "Depth handles in the gap under each modulated knob").
        1. Show as many as fit the knob's (label's, cell's) width; past that, the last place becomes a "+n" pill.
        2. Hovering or clicking the pill lists the rest in a small popup, each with its depth handle (drag, bypass,
           remove); once the MODS view exists, it can open that instead, filtered to the target.
        3. uishot: eight sources on one knob, nothing drawn outside its column (read off the pixels).


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
      grid). features.md doesn't say "depth handle" yet: it calls them small dots, in the Modulation section.
