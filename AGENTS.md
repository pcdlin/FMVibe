# AGENTS.md

Notes for anyone (human or agent) working on FMVibe, a six-operator FM synthesizer as a CLAP instrument plugin
for Windows. The README introduces the plugin and says how to build and install it. What the plugin does and how
its editor works, in full, is in [docs/features.md](docs/features.md): read the parts you need before changing a
feature.

## Ground rules

- Parameter ids, the wave, filter type, effect type, source type and fixed Mult lists, and the state layout are
  stored in host projects and preset files: follow the rules under [Plugin facts](#plugin-facts) when touching them.
- Keep `ctest` green (smoke test, audio quality, UI snapshots). New editor interactions get a replay in
  `tools/uishot.cpp`, new engine behavior a check in `tests/smoke_host.cpp` or `tests/quality.cpp`.
- When a feature changes, update [docs/features.md](docs/features.md); keep `README.md` to its short
  description and the build and install instructions.
- Don't start Bitwig or another host yourself: the plugin is tried in the host by hand.
- The editor draws through `Canvas`, which has two backends: Direct2D (`src/gui/canvas.cpp`) and the browser
  demo's 2D context (`src/gui/canvas_web.cpp` with `web/site/canvas2d.js`). A new drawing call needs both.
- Text the plugin shows (labels, tooltips, menus, messages, parameter names, remote control pages) is worded in
  `src/uistrings.inc` and looked up by name: `tr(Str::Name)`, or `trf()` for a wording with `{1}`, `{2}` in it. New
  text gets a line there. Tests that match on text (menu entries, page names) look it up the same way, so a
  rewording keeps them green.

## Build

What you need, `build.ps1` (which runs the tests below with every build, unless `-SkipTests` builds only the plugin),
the plain CMake commands and installing into a host are in the [README](README.md#build).

## Testing

`ctest` runs three programs:

- **Smoke test** (`tests/smoke_host.cpp`): a small headless host. It loads the `.clap` and checks the parameter
  table, text round-trips, the note source's range (every MIDI note, on to the pitch at 20 kHz) and remote-control pages. It then plays
  notes, checking:
  - sound comes out and `NOTE_END` arrives after the release;
  - voice stealing and choke;
  - per-voice host modulation, and a mod-envelope slot driving its target, and running its release out after
    the operators have faded (the voice reporting `NOTE_END` only then);
  - spectrum checks on an A4: the matrix produces FM sidebands; every wave sounds; a saw's 2nd harmonic sits at
    half the fundamental, and Alt Sine's, Organ's and Vowel's where their series put them; a Custom wave plays its
    partials, only as many as its count (the 40th silent at 32, heard at 64); a low-pass at 300 Hz and a high-pass
    at 2 kHz move that ratio the right way, each slope from 6 to 24 dB harder than the one before (the 6 dB
    low-pass a one-pole), and a band-pass set to 6 or 18 dB plays 12 or 24;
  - every filter type stays finite at high resonance at every slope, and a voice ringing through a resonant comb
    still ends;
  - a fixed operator playing its Freq × Mult (at ×0.1 and ×10 too) for a C3 and an A4, under a bend, with its
    Pitch and Detune turned and modulated;
  - a new instance has only operator A on, no filter key tracking or going out, its shaper effects linked, its
    frequency shifters' range at 100 Hz, its flangers stereo and its Custom waves at 32 partials; a switched-off
    operator is silent, and a switched-off filter passes nothing; a filter routed nowhere isn't heard, and goes out
    by its Out; filter 1 feeds filter 2;
  - a shaper's output modulates an operator (and one that is off doesn't), a hard shaper into a filter squares a
    sine off, and a shaper routed nowhere isn't heard but goes straight out by its To Out; a filter into a hard
    shaper squares it off too, and a filter and a shaper feeding each other in a loop still end with their note,
    and stay finite when the loop lifts itself past unity;
  - the mod wheel moves nothing until it is assigned, and then its target, like a macro;
  - a MIDI source following its controller (and not another), on pressure the channel's and a note's own
    (polyphonic aftertouch or CLAP's pressure expression, not another key's), and on slide resting in the middle and
    following a note's brightness expression; Slew taking a controller turned up mid-note slowly, and a Random source
    giving each note a level of its own;
  - MPE over MIDI: with it on, a member channel's pitch bend bending its note alone and the master channel's every
    note, and a member channel's pressure being its note's alone (the master channel's standing in until it sends
    some), in mono too for a note taking the voice over; with it off, any channel's bend bending every note;
  - an LFO's Phase moving where notes start it (a one-shot running from there to its cycle's end), while a free
    one keeps its clock;
  - bipolar sources swing their targets both ways: an envelope about its middle level, a macro, the mod wheel and
    the note source about the middle of what they put out;
  - the note source, its curve drawn along the cutoff knobs' as it starts, tunes a notch at 0 to an A4, and
    follows a pitch bend; bipolar, it does the same from the cutoff a filter starts with; its range and curve shape
    what it puts out;
  - velocity taking its target as far as the note's velocity, in a straight line as it starts; bipolar, about the
    middle of what it puts out; its curve drawn the other way round reversing it;
  - a slot's curve bending what its source puts out both ways (a macro's, velocity's, and a macro's that its aux
    runs per voice), a bipolar one keeping its middle still;
  - a slot's Amount scaling its depth, and its aux: velocity scaling a macro's slot in each voice, the mod wheel down
    leaving its target alone; a macro's and velocity's slots bypassed leaving theirs alone; velocity and the note
    source scaling an LFO's slot as its aux from a note's first block;
  - each effect on its own, among them a shimmer's tail rising in octaves where a hall's has none (a fifth up or
    an octave down as its Pitch says, and nothing shifted at 0 % Shimmer), the widener
    widening the sides above its high-pass while the mono sum stays dry, the phaser's and flanger's notches deepest
    at 100 % mix (half wet), the flanger's taking half of the tone in it away at 50 %, a stereo flanger's sides
    sweeping apart and a mono one's together, a linked shaper keeping its level, a symmetric one shaping its
    lower half as its upper one (a drawn curve losing its 2nd harmonic, Asymmetric left as it is), and
    the filter effect's key tracking putting a cutoff at C4 onto an A4, and the frequency shifter moving by its
    Shift's share of its Range;
  - unison copies spread across the stereo field (and match when centered);
  - mono: a new key takes the voice over, letting go returns to the held key, and every note reports `NOTE_END`
    exactly once;
  - legato keeps the envelope at its sustain level while retrigger restarts the attack, in mono and in poly, where
    a note played over a held one joins that one's envelopes (at its sustain, or swelling along with it); glide
    slides between
    overlapping notes and skips detached ones unless Always is on (checked by tracking the pitch);
  - a lower voice limit applies immediately, while a higher one requests a restart and is reported through
    voice-info after reactivation.

  It opens the editor inside a hidden host window and drags the Volume knob with mouse messages. It checks that
  the param changes and that the host receives gesture-begin, value and gesture-end events, and that an older
  value the host hands back right after isn't taken. It saves/loads state
  through a stream that only moves 5 bytes per call, a version 1 state's Volume loading 12 dB lower, a version 2
  state's Legato loading off in poly (and as it was in mono), envelope times
  stored past 10 s loading as 10 s, a layout as large as one gets among them (and one of NaNs,
  which loads as the defaults), and checks that
  every remote control page has an id of its own. It also reports CPU cost: 16 voices × 6 operators take about a third of what they took
  before the engine's per-block route lists and shared params, which on the development machine was about 22 % of
  one core at the default 2x oversampling and about 38 % with every operator at 8 unison copies (now roughly 7–8 %
  and 25 %; the smoke test prints the figures).
- **Audio quality** (`tests/quality.cpp`): drives the engine directly and measures, at every oversampling factor:
  - the decimator's passband ripple and alias rejection;
  - aliasing (the energy off the harmonics, 20 Hz–20 kHz) of a high saw, bright FM, feedback and a driven filter;
  - zipper noise from an LFO on an operator's level, a matrix amount and a filter cutoff;
  - clicks when a voice is stolen, when a note joins a held one in poly legato (fading in over 5 ms, its Random
    LFOs carrying on with the held one's step), and when a param (Volume, and an effect's: a compressor's Makeup, a delay's and a
    reverb's Mix, a DJ EQ band) jumps while events split the block into single frames;
  - the reverb's tails: every algorithm's late spectrum without lasting resonances and its two sides unalike, an
    undamped tail falling 60 dB around 1 kHz in its decay time, and a reverb with a 200 ms pre-delay ringing on
    until its tail has been heard, and stopping after;
  - a quiet, slow sine through a drawn shaper curve coming out as drawn, without spikes where it crosses the
    curve's table steps;
  - the random LFO's steps and glides, the phaser's and flanger's sweeps synced to the host tempo, the parametric
    EQ's, the phase disperser's and the voice filters' analyzers, and that each example preset holds valid values
    and sounds at a sane level;
  - each factory preset's content (the params it sets, by id, its node places and macro names) against its
    checksum in `kPresetChecksums`, so an edit can't change a preset unnoticed; a preset changed on purpose, or a
    new one, gets the checksum the check prints;
  - the mod wheel leaving the FM alone until it is assigned, and the output meter's peaks, fall and clip light;
  - Volume setting the level after the effects, and a mod envelope's level half way through its release;
  - every note reporting its end once: in mono a note taken over in its release, with note ids a note on a key
    struck again while it rings, and the voices a lower voice limit cuts; a velocity or note expression that isn't
    a number staying finite; host modulation of a macro, the mod wheel, the note source's depth and velocity's
    moving their targets; a comb never reading past its line; a voice's right channel coming back from mono where
    the left is;
    and an envelope's curve bent again after a straight stretch carrying on along it;
  - an operator's Phase starting its wave that far into its cycle, set or modulated;
  - the scope capturing nothing until asked, then an operator's cycles from where they start (a sine's a sine, a
    square's a square), standing still from one span to the next under FM, two cycles differing under a modulator
    at half the ratio, a slow span shown as it fills, nothing again once it isn't asked for, and a high operator's
    (a cycle of 4.5 samples) for little more than the voice costs;
  - the CPU cost of each factor.
- **UI snapshots** (`tools/uishot.cpp`): renders the editor offscreen to PNGs in `build\<config>\ui-shots`. On the
  way it replays drags and clicks:
  - assigning LFOs and envelopes to a knob, a header knob and a wire label, and turning a knob;
  - connecting operators by dragging from a node's edge (lit under the mouse), anywhere around it, while a click
    there only picks the node and a drag from further inside moves it; a filter going out by its edge dragged onto
    the bus, with nothing going in, and stopping by its arrowhead dragged away; routing a carrier through a filter,
    which goes out in its place, and picking waves and filter types;
  - moving routes by dragging their arrowheads onto another node and onto the OUT bus, with their modulation, and
    removing one by letting its arrowhead go in empty space;
  - the MODS tab: a row per modulation, its Amount bar and aux's × changing it, and the switch after its source's
    name the source's polarity; its curve bent up and down by a drag, undoably, drawn in its source's color (read
    off the rendered pixels), and straightened by a double-click; + Aux's, the target's (three levels deep, into an operator's params) and the source's
    cascading menus picking an aux, a target and a source, the modulation keeping its depth, Amount and aux as it
    moves, undoably, and what modulates its depth following it (or, onto a macro or when it is removed, going); the wheel scrolling the rows; a row's typed depth, Edit depth and bypass dot (a macro's too); and a source's
    handle dropped on a depth handle becoming that modulation's aux, ringed in its color, the macro then no longer
    faint (read off the rendered pixels), and the depth handle hovered drawing the line from its source's handle
    (read off the rendered pixels);
  - the MODS tab's columns' names with no modulations (read off the rendered pixels);
  - the MODS view's filter: its magnifier opening it to take the keyboard, the words typed (every one, in any case)
    narrowing the rows, its × emptying it, Escape and a click elsewhere letting the keyboard go; and the target
    menu's search listing the targets found with their path, Up and Down picking one, Enter taking it, Escape
    emptying it and then closing the menu; an operator's targets in two columns, the envelope's beside the pitch's;
  - the modulation panel's title row adding Random and MIDI sources, each with its card, a MIDI card's knob picking
    its controller and its input's list pressure or slide instead, its CC knob going, and the list button's tooltip
    following its input; the MPE switch beside the Pan knob, and its menu picking the bend range;
  - adding an empty modulation in the MODS tab: it opens its source's menu, takes a source and modulates nothing,
    then a target from Global (which offers no Mod Wheel) makes it its source's modulation, each pick undoable, and
    its × removes it; one given a source that has rows going among them, its source menu, open over the rows,
    lighting none of them (read off the rendered pixels); a source card's button showing its modulations in MODS, or with none adding an empty one
    from it;
  - bending a wire by dragging it sideways (lit under the mouse, its label following its middle), either way,
    undoably, back to its first bow by a double-click or Reset Bend, its bend going with it when it is
    disconnected, and one along the panel's top bowing only as far as keeps it and its label inside (read off the
    rendered pixels);
  - an operator lighting up while modulation opens its route from 0 % (read off the rendered pixels);
  - a switched-off source's handle going faint, and a macro's and the note source's until they modulate something
    (read off the rendered pixels);
  - the dot on a filter's and a shaper's tab while it sounds out, and the output meter and its clip light (read off
    the rendered pixels);
  - a shaper: switching it on with its tab, sending an operator and a filter into it (in the graph and the matrix)
    and running it onto an operator and into a filter, its page's curve list, link switch, symmetric switch (for
    a Custom curve) and knobs, its Custom curve drawn on the page and in the big editor;
  - the shaper effect's SYM switch, hidden but for a Custom curve, putting a point in the middle of its drawn curve, a double-click on the curve's
    mirrored left half adding a point on the right, the middle point refusing to be deleted, and the switch
    undoing with its point;
  - dragging the filter response handle, switching between the graph and matrix views (the tab shown opening its
    panel's edge, and a hovered one's shape, read off the rendered pixels), and toggling a matrix cell;
  - picking a filter's slope from its list, a band-pass's 6 and 18 dB refused;
  - making sources unipolar and bipolar from their menus (an envelope's and an LFO's card, a macro's knob and
    handle, the mod wheel and the note source's display);
  - dragging a Param EQ band's handle, and Alt-dragging it for its Q;
  - dragging an envelope's handles, past the display's end too, where the envelope zooms out to stay inside (read
    off the rendered pixels), and straightening a segment's bend to 0 % with a double-click, its readout drawn
    over the curve (read off the rendered pixels);
  - an operator page's ENVELOPE, UNISON and SCOPE tabs: the unison knobs on theirs, the scope asking for its
    operator's output (and a noise operator's, or a hidden one's, for none), its trace drawn over 2 and 4 cycles
    and as a slow span fills, the faint plain wave with no note, a hidden tab's dot while one of its knobs is
    modulated (read off the rendered pixels), and a dragged handle opening the tab it is held over;
  - an operator's fixed pitch switched on by its keyboard, Freq and Mult set to where the ratio was in one undo
    step and taking Ratio's and Fine's places, Mult stepping, and Pitch and Detune dimmed (read off the rendered
    pixels) and not turning;
  - switching operators and filters on and off with their tabs, the matrix's names and their ghosts in the graph,
    and off from a node's right-click menu, a switched-off one's page dimmed (read off the rendered pixels);
  - the play switches and the glide knob;
  - an LFO starting synced in quarter notes, and its sync switch freeing it; its Phase knob, marked on its display
    and faint while the LFO is free (read off the rendered pixels);
  - the newer effects' cards (the widener's knobs, the frequency shifter's Shift, the phaser's SYNC, the
    flanger's STEREO, the filter's Key Track and the shaper's and delay's LINK too) and the list of effects with
    their icons;
  - a filter's and the filter effect's responses moving with the note their key tracking follows (read off the
    rendered pixels);
  - switching effects with their cards' icons (a switched-off one's staying grey under the mouse, read off the
    rendered pixels), dragging cards by them, and folding cards, the icon staying put;
  - the right-click menu on knobs;
  - the macros' knobs (a double-click setting a bipolar one to its middle), handles and names, the note source's
    and velocity's handles, displays and curve editors, velocity's handle faint until it modulates something (read
    off the rendered pixels), and the mod wheel's
    handle (new modulation starting upward); with no note playing, a knob's dot showing where a macro takes it
    (read off the rendered pixels); the octave shift's small buttons under the keys; collapsing the keyboard strip
    to its top row by its chevron or title, the mod wheel's handle moving into it and still mapping the wheel, and
    Bend's knob, over the bend wheel, staying in it and turning;
  - the dashed cards that add effects and sources (opening the list of types over themselves, the effects' staying
    over a switched-off card's dimming at the chain's right end, read off the rendered pixels; the sources' closing
    at a click around it, and shutting with the panel for good), and the arrow between two effects adding one there;
  - picking the oversampling and the theme, and dragging the glow slider, in the settings dialog (with the editor
    rendered in each theme, and glowing), and closing it with its x or a click around it;
  - undo and redo of drags, wheel runs, node moves, typed values and a removed effect with its modulation;
  - typing values into knobs, header knobs and target depths, with Enter, Escape and a click elsewhere;
  - the preset bar's arrows (asking first while the patch has changed, the dialog a line shorter before a file
    loads) and the preset browser, which loading a preset closes; its search narrowing the list as words are
    typed, Enter loading the preset found, Up and Down picking one, Escape and the search's × emptying it, and
    its editing: selecting with Shift and the arrows, Home and End, the mouse (a drag, a Shift+click, a
    double-click for a word), cutting and pasting;
  - drawing a Custom wave's partials, at 16 and 64 of them too, the ones a lower count hides kept as they were, the
    edit button (a pencil) by the keyboard switch opening the partial editor and closing it (read off the rendered
    pixels), and the LFO shape editor hiding the inspector;
  - an LFO's shape list, the random LFO's knobs and its dot waiting at the end of a cycle (read off the rendered
    pixels), the list of more waves (Organ among them), the shape editor's grid, snapping and reset button, and
    scrolling the source cards by holding a dragged handle at the panel's edge;
  - scrolling the effects and the sources by holding a card being moved at either end of its row, where it lands;
  - dragging the effects, the source cards, the MODS view's rows and the preset browser's list with the middle
    button held: over a knob without turning it, stopping at the row's end and following back from there at once,
    and doing nothing while the left button drags a knob;
  - eight modulations on a knob and six on a matrix cell showing as many depth handles as fit and a "+n" pill,
    nothing drawn outside the knob's column or in the next cell (read off the rendered pixels); the pill's list
    bypassing, setting the depth of and removing its modulations and closing at a click elsewhere, opening under a
    macro's handle held over the pill and taking it as a row's aux, and a source's line ending at the pill holding
    its handle;
  - a hovered depth handle drawing its range wider, over the others' (read off the rendered pixels);
  - an LFO's handle showing its "Drag to assign" tooltip only until it modulates something (read off the rendered
    pixels).

  Pass a scale to render larger: `build\release\fmvibe-uishot.exe out 2`.

More checks:

- **clap-validator**: the free-audio conformance test suite, run with `.\build.ps1 -Validate`.
- **In Bitwig Studio**: install the plugin as the [README](README.md#build) describes; Bitwig finds it
  in the per-user CLAP folder.

## Browser demo

The engine and the editor also build for the browser with Emscripten, as a demo page that can be served from any
static host (GitHub Pages, say). The page needs no cross-origin isolation.

```powershell
.\build-web.ps1                # -> build\web\site
.\build-web.ps1 -Serve         # and serve it on http://localhost:8080 (-Port to pick another)
```

On its first run, `build-web.ps1` fetches emsdk into `build\emsdk` (about 1 GB) and installs the Emscripten
version pinned at its top. After that it loads Visual Studio's environment for CMake and Ninja and builds the
`web` preset.

- It builds two modules. `fmvibe-dsp.wasm` (`web/dsp.cpp`) is a standalone module with C exports: the synth,
  which `web/site/worklet.js` runs in an AudioWorklet. `fmvibe-ui` (`web/ui.cpp`) is the View and a `Controller`
  on the page's main thread.
- The two modules only send each other messages: the param edits, the notes and the telemetry, whose struct
  bytes cross unchanged.
- Presets are the factory ones, held in memory. Load, Save and the preset folder show a message instead, since the
  demo has no files. The theme and glow are kept in the browser's storage.
- The computer keyboard plays notes: A W S E D … from middle C, with Z and X to shift the octave.
- The page polls and paints at most about 60 times a second, whatever the display's rate, and, like the plugin
  window, repaints on a mouse move only when what the editor shows changes.
- The page ships FMVibe's `LICENSE` (as `LICENSE.txt`) and `web/site/THIRD-PARTY-NOTICES.txt`, the licenses of the
  code its modules hold from CLAP, Emscripten and musl, both linked from its footer. When `build-web.ps1` pins
  another Emscripten version, the notices take that version's `LICENSE` and `system/lib/libc/musl/COPYRIGHT`.
- WebAssembly can't flush denormals to zero, which `Plugin::process()` does. Instead, the effects' input, the
  voice filters' input and the decimators get a signal far below hearing, behind `__EMSCRIPTEN__`. Without it,
  tails ringing out cost many times the CPU. The plugin's build is unchanged.

## Releasing

A version tag publishes a release. `.github/workflows/release.yml` builds the plugin on Windows (Visual Studio
2026) with `build.ps1 -Validate`, so every test and clap-validator have to pass. It puts `FMVibe.clap`, `LICENSE`
and the CLAP SDK's license (`LICENSE-CLAP`) into `FMVibe-<tag>-windows-x64.zip`, and creates a GitHub release
for the tag with that zip and notes generated from the commits since the previous tag. Meanwhile it builds the
browser demo on Linux, with the Emscripten version `build-web.ps1` pins, and publishes it on GitHub Pages
(`https://pcdlin.github.io/FMVibe/`). Neither goes out unless both build.

1. Set the new version in `project()` at the top of `CMakeLists.txt` (and where `docs/features.md` names it), and
   commit.
2. Tag that commit with the same version and a `v` before it, and push the tag:

   ```powershell
   git tag v0.2
   git push origin v0.2
   ```

The workflow refuses a tag that doesn't match the version in `CMakeLists.txt`. If a UI check fails on GitHub's
runner, the run keeps the editor's snapshots as its `ui-shots` artifact.

Before the first release, the repository needs two settings:

- *Settings > Pages > Build and deployment*: Source **GitHub Actions**.
- *Settings > Environments > github-pages > Deployment branches and tags*: add a tag rule for `v*`. GitHub creates
  this environment allowing only the default branch, and a deploy from a tag is refused until it is added.

On GitHub's free plan, Pages needs the repository to be public.

## Debugging in a host

Install the debug build (`.\build.ps1 -Config debug -Install`). Bitwig runs plugins in a separate process, so in
Visual Studio use *Debug > Attach to Process* and pick `BitwigPluginHost-X64-AVX2.exe`; breakpoints in `src/` then
hit.

## Layout

| Path | What |
| --- | --- |
| `src/entry.cpp` | Exported `clap_entry` and the plugin factory |
| `src/plugin.cpp` | The CLAP plugin: the Plugin class and its extension tables (process loop, events, ports, params, state, remote controls, voice info, gui) |
| `src/state.cpp` | The state's bytes: the patch (params, then the editor layout chunk, whose fields one list describes for writing and reading), the settings and preset chunks; independent of Windows |
| `src/remotepages.cpp` | The remote control pages hosts show, built from the patch and the editor's source order |
| `src/presets.cpp` | The preset library: the preset folder, its scan, the examples written into it |
| `src/usersettings.cpp` | The user's settings file in the roaming app data (theme, glow, examples given) |
| `src/sharedtelemetry.h` | The telemetry the audio thread hands the editor, in atomics |
| `src/params.h` | Parameter layout (index ↔ id), slot targets, value mappings |
| `src/params.cpp` | Parameter table (names, ranges, defaults) and value text |
| `src/uistrings.inc` | The wording of every piece of text the plugin shows, by name; `src/uistrings.h` looks it up |
| `src/synth.cpp` | FM voices, matrix, filter routing, LFOs and mod envelopes, telemetry; independent of CLAP |
| `src/wavetable.cpp` | Band-limited operator wavetables, one per octave |
| `src/filter.h` | State-variable and comb filters, plus their response curves for the editor |
| `src/clipcurve.h` | The shapers' curves, anti-aliasing and running, shared by the shaper effect and the voices' shapers |
| `src/analyzer.cpp` | The spectrum analyzer (FFT) behind the Param EQ's, filter effect's, phase disperser's and voice filters' displays |
| `src/envelope.h` | AHDSR used by operators and mod envelopes |
| `src/gui/view.cpp` | The editor: its widgets, drawing and mouse and keyboard handling, and the model access behind them. Window-free, so it also renders offscreen |
| `src/gui/theme.h`, `src/gui/theme.cpp` | The editor's color themes, the colors of the one being painted, and the graphs' glow |
| `src/gui/layout.h` | Where the editor's panels, cards, buttons, lists and dialogs go, in logical pixels |
| `src/gui/icons.h`, `src/gui/icons.cpp` | The editor's icons (effects, filter types, band shapes, LFO modes, waves) and the point cache behind them and the graphs' curves |
| `src/gui/routing.h`, `src/gui/routing.cpp` | The routing graph: which param joins two nodes, wires and arrowheads; connecting and rerouting |
| `src/gui/shapes.cpp` | Editing drawn shapes: freeform LFOs, shaper curves and the note source's curve |
| `src/gui/mods.cpp` | The MODS view: every modulation as a row, its filter, and the cascading menus (the target's with a search) that move it, retarget it and pick its aux |
| `src/gui/dialogs.cpp` | The settings and confirm dialogs, the preset browser with its search, and the text fields' editing |
| `src/gui/canvas.cpp` | Small Direct2D/DirectWrite drawing layer |
| `src/gui/canvas_web.cpp` | The same drawing layer over the browser's 2D context, for the demo |
| `src/gui/editor.cpp` | Win32 child window that hosts the view in the plugin window |
| `src/gui/controller.h` | What the editor needs from the plugin (values, edits, telemetry, layout) |
| `src/gui/controller.cpp` | The undo history, which the plugin keeps for the editor |
| `src/factory.cpp` | The example presets written into the preset folder |
| `tests/smoke_host.cpp` | Headless test host |
| `tools/uishot.cpp` | Offscreen editor renderer and interaction checks |
| `build.ps1` | One-shot configure/build/test/validate/install |
| `web/dsp.cpp`, `web/ui.cpp` | The browser demo's audio and editor modules |
| `web/site/` | The demo page: the canvas and input (`main.js`), drawing (`canvas2d.js`), the AudioWorklet (`worklet.js`) |
| `build-web.ps1` | Builds the browser demo with Emscripten, and serves it |
| `.github/workflows/release.yml` | On a version tag: a GitHub release with the plugin, and the browser demo on GitHub Pages |

Everything except `entry.cpp`, `plugin.cpp`, `presets.cpp`, `usersettings.cpp` and `editor.cpp` builds into the
`fmvibe-core` static library, which
the plugin and the tools share. The browser demo builds the engine (`FMVIBE_ENGINE_SOURCES`) into its modules
itself.

## Plugin facts

- CLAP id `io.github.pcdlin.fmvibe`. Changing it makes hosts treat it as a different plugin.
- The version number is set once, in `project()` at the top of `CMakeLists.txt`. It reaches the code as
  `FMVIBE_VERSION`: the plugin descriptor reports it to hosts and the settings dialog shows it. FMVibe is released
  under the MIT license (`LICENSE`).
- Parameter ids are stored in host projects and automation. `src/params.h` derives them per section with spare
  room: globals 0+, operators 100 + 50·op (their filter and shaper sends, then Fixed, Freq, Mult and Phase; the
  9th field, once Velocity, does nothing and is hidden, kept so the ids after it stay), matrix
  400 + 10·from + to, filter modulation
  500 + 10·filter + to, shapers 600 + 50·shaper, filters 800 + 20·filter, filter links 900+, effect slots
  1400 + 80·slot (their fields past
  the 80th 12000 + 20·slot), source places
  2800 + 40·place, freeform shapes 4000 + 100·place (their points past the 33rd 15000 + 100·place), the sources'
  later slots 6000 + 30·place, shaper effects' curves 7000 + 50·slot (their points past the 16th
  17000 + 200·slot), slot bypasses 9000 + 20·place (then the macros', the note source's, the mod wheel's and
  velocity's, 9320 + 20 a source by the sources' number), Custom waves' first 32 partials 10000 + 100·op, macros
  11000 + 40·macro, the note source 11400+ (its slots, then its range and curve), the mod wheel's slots 11500+,
  velocity 11600+ (its slots, then its curve and Bipolar), the
  Custom waves' partial counts 14000+ and their partials past the 32nd 14100 + 100·op, the voice shapers'
  curves 13000 + 50·shaper (their points past the 16th 21000 + 200·shaper), every slot's aux and Amount
  22000 + 40·source (a pair per slot, by the sources' number: the places, the macros, the note source, the mod wheel
  and velocity; a source added later takes the next block), and every slot's curve 24000 + 20·source (by the
  sources' number likewise).
  Once released, new fields go at the end of their section's
  enum and fields are never reordered. The same goes for the wave, filter type, effect type, source type, slot aux
  field and fixed Mult lists, and the routing views,
  which are stored by number. `src/params.h` pins each section's first and last ids, the counts they come from and
  the stored lists' members with static_asserts, so a change that renumbers them doesn't compile; a field added to
  `FxParam` also needs its effect type in `kFxFieldRuns`.
- Like an effect slot, each of the 16 source places has the fields of every type (an LFO's, an envelope's, and
  Slew, CC, Bipolar and Input for Random and MIDI), and a Type param (Empty, LFO, Envelope, Random or MIDI) that
  picks which are used (`sourceFieldUsedBy()`). Hosts name the fields by place ("Mod 3 Rate"), since a name can't
  follow the type; fields of the other types are hidden from them. A MIDI source takes CLAP MIDI events
  (controllers, channel and polyphonic aftertouch) and CLAP's pressure and brightness note expressions, as its Input
  picks.
- A slot's *target* param stores the target's id + 1 (0 = unused). Targets aren't offered for automation, since
  they are wiring, but they are saved like any other param. Their depths are normal automatable params. Each slot
  also has an aux (the source + 1 that scales it, 0 = none; wiring, hidden) and an Amount (0–1, 1 to begin with,
  automatable), whose defaults leave a slot as it was. A slot swings its target one way or both as its source's
  own switch says.
- Edits in the editor reach the host as `PARAM_GESTURE_BEGIN` / `PARAM_VALUE` / `PARAM_GESTURE_END` events. Hosts
  record automation and undo from these, and mark the project as changed. Loading a preset, or Init, sends them for
  each param whose value changed. The last of the queue they wait in is kept for the ends of gestures already begun,
  so a host is never left inside one; an edit that doesn't fit has the host read every value again (a values
  rescan) instead. For half a second after the plugin
  tells the host a param's value, it takes no value for that param from the host: Bitwig hands values back a
  little later, and taking them would flip a preset loaded meanwhile back to the one before, for a moment.
- The state blob holds `(id, value)` pairs with a version number (currently 3: version 1's Volume loads 12 dB
  lower, since Volume now gives back the 12 dB the voices go into the effects with; before version 3, a state in
  poly loads Legato off, since it did nothing there then and acts there now), followed by the editor layout:
  node, filter and shaper positions, selected node, zoom, the routing view, whether the modulation panel is collapsed,
  the order the sources show in, the routes kept at 0 %, the shape editor's grid, the macros' names, which
  effect and source cards are folded, whether the keyboard strip is collapsed, the operator page's tab, the
  scope's cycle count, the wires bent away from their first bow (by route) and the MODS view's empty modulations
  (each its source + 1 and its target's id + 1, 0 for none). The host's state goes on with
  the settings chunk and a preset chunk (whether the patch changed since its preset, then the preset file's path
  in UTF-8); preset files stop after the layout. Params and layout fields a
  state doesn't have keep their defaults; a layout or settings chunk longer than the loader knows (up to 4096
  floats are read) keeps the fields it knows and is skipped past, so the chunks after it still load; a chunk cut
  short keeps the floats that arrived whole, and a layout value that isn't a number keeps its default. States
  with a newer version are refused.
- Times and LFO rates are stored as 0–1 on a log curve (1 ms–20 s, 0.02–100 Hz), so host knobs and modulation
  have resolution where it matters. The value text shows ms/s/Hz. Envelopes' attacks, holds, decays and releases
  stop at 10 s (`kMaxEnvTime`), their range ending short of the curve's top; a state or preset holding a longer one
  loads it as 10 s, since loading holds every param within its range.
- The plugin takes CLAP note events (preferred), raw MIDI and MPE MIDI (see the MPE switch, `GlobalParam::Mpe`:
  with it on, channels 2 to 16 bend, press and slide their own notes, kept per channel in the synth), and reports
  `NOTE_END` so hosts with
  polyphonic modulation (Bitwig) know when a voice has finished. In mono, a note that is taken over keeps its
  host voice while its key is down and reports `NOTE_END` when the key is let go. Per-note modulation follows the
  note that is sounding.
- Voices are allocated when the host activates the plugin, for the current Voices value; the audio thread never
  allocates. Lowering the limit takes effect at once: the voices past it fade out as stolen ones do. Raising it past what is allocated calls
  `host->request_restart()`, and the host then deactivates and reactivates the plugin. Each voice takes about
  66 KB at 48 kHz, mostly the two comb filters' delay lines, which grow with the oversampling: at the default
  2x, about 105 KB, so 512 voices use about 54 MB. Voices isn't automatable for the same reason.
- The CRT is linked statically. The `.clap` depends only on Windows system DLLs (kernel32, user32, Direct2D,
  DirectWrite), not on the VC++ redistributable.

## Updating the CLAP SDK

Change the tag in the `FetchContent_Declare(clap ...)` URL in `CMakeLists.txt` and replace the hash with the new
archive's SHA-256 (`Get-FileHash <file> -Algorithm SHA256`).

## Next steps

- Presets: the `clap.preset-load` extension and preset discovery, so hosts can browse FMVibe's presets too.
- Sound: fixed-frequency operators, key scaling, oversampling for high-index patches.
- VST3, AU and standalone builds: [clap-wrapper](https://github.com/free-audio/clap-wrapper) builds these from the
  same CLAP code. The plugin's editor is Windows-only for now (the browser demo has its own drawing backend).