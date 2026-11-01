# AGENTS.md

Notes for anyone (human or agent) working on FMVibe, a six-operator FM synthesizer as a CLAP instrument plugin
for Windows. The README introduces the plugin and says how to build and install it. What the plugin does and how
its editor works, in full, is in [docs/features.md](docs/features.md): read the parts you need before changing a
feature.

## Ground rules

- Parameter ids, the wave, filter type, effect type and source type lists, and the state layout are stored in host
  projects and preset files: follow the rules under [Plugin facts](#plugin-facts) when touching them.
- Keep `ctest` green (smoke test, audio quality, UI snapshots). New editor interactions get a replay in
  `tools/uishot.cpp`, new engine behavior a check in `tests/smoke_host.cpp` or `tests/quality.cpp`.
- When a feature changes, update [docs/features.md](docs/features.md); keep `README.md` to its short
  description and the build and install instructions.
- Don't start Bitwig or another host yourself: the plugin is tried in the host by hand.
- The editor draws through `Canvas`, which has two backends: Direct2D (`src/gui/canvas.cpp`) and the browser
  demo's 2D context (`src/gui/canvas_web.cpp` with `web/site/canvas2d.js`). A new drawing call needs both.

## Build

What you need, `build.ps1` (which runs the tests below with every build, unless `-SkipTests` builds only the plugin),
the plain CMake commands and installing into a host are in the [README](README.md#build).

## Testing

`ctest` runs three programs:

- **Smoke test** (`tests/smoke_host.cpp`): a small headless host. It loads the `.clap` and checks the parameter
  table, text round-trips, the note source's range (every MIDI note) and remote-control pages. It then plays
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
  - a new instance has only operator A on, no filter key tracking or going out, its shaper effects linked, its
    frequency shifters' range at 100 Hz, its flangers stereo and its Custom waves at 32 partials; a switched-off
    operator is silent, and a switched-off filter passes nothing; a filter routed nowhere isn't heard, and goes out
    by its Out; filter 1 feeds filter 2;
  - a shaper's output modulates an operator (and one that is off doesn't), a hard shaper into a filter squares a
    sine off, and a shaper routed nowhere isn't heard but goes straight out by its To Out;
  - the mod wheel moves nothing until it is assigned, and then its target, like a macro;
  - bipolar sources swing their targets both ways: an envelope about its middle level, a macro, the mod wheel and
    the note source about the middle of what they put out;
  - the note source, its curve drawn along the cutoff knobs', tunes a notch at 0 to an A4, and follows a pitch
    bend; its range and curve shape what it puts out, a straight line over every MIDI note as it starts;
  - each effect on its own, among them a shimmer's tail rising in octaves where a hall's has none (a fifth up or
    an octave down as its Pitch says, and nothing shifted at 0 % Shimmer), the widener
    widening the sides above its high-pass while the mono sum stays dry, the phaser's and flanger's notches deepest
    at 100 % mix (half wet), the flanger's taking half of the tone in it away at 50 %, a stereo flanger's sides
    sweeping apart and a mono one's together, a linked shaper keeping its level, and
    the filter effect's key tracking putting a cutoff at C4 onto an A4, and the frequency shifter moving by its
    Shift's share of its Range;
  - unison copies spread across the stereo field (and match when centered);
  - mono: a new key takes the voice over, letting go returns to the held key, and every note reports `NOTE_END`
    exactly once;
  - legato keeps the envelope at its sustain level while retrigger restarts the attack; glide slides between
    overlapping notes and skips detached ones unless Always is on (checked by tracking the pitch);
  - a lower voice limit applies immediately, while a higher one requests a restart and is reported through
    voice-info after reactivation.

  It opens the editor inside a hidden host window and drags the Volume knob with mouse messages. It checks that
  the param changes and that the host receives gesture-begin, value and gesture-end events. It saves/loads state
  through a stream that only moves 5 bytes per call, a layout as large as one gets among them, and checks that
  every remote control page has an id of its own. It also reports CPU cost: on the development machine, 16
  voices × 6 operators take about 22 % of one core at the default 2x oversampling, and about 38 % with every
  operator at 8 unison copies.
- **Audio quality** (`tests/quality.cpp`): drives the engine directly and measures, at every oversampling factor:
  - the decimator's passband ripple and alias rejection;
  - aliasing (the energy off the harmonics, 20 Hz–20 kHz) of a high saw, bright FM, feedback and a driven filter;
  - zipper noise from an LFO on an operator's level, a matrix amount and a filter cutoff;
  - clicks when a voice is stolen, and when a param jumps while events split the block into single frames;
  - the reverb's tails: every algorithm's late spectrum without lasting resonances and its two sides unalike, and
    an undamped tail falling 60 dB around 1 kHz in its decay time;
  - the random LFO's steps and glides, the phaser's and flanger's sweeps synced to the host tempo, the parametric
    EQ's and the voice filters' analyzers, and that each example preset holds valid values and sounds at a sane
    level;
  - the mod wheel leaving the FM alone until it is assigned, and the output meter's peaks, fall and clip light;
  - Volume setting the level after the effects, and a mod envelope's level half way through its release;
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
  - a switched-off source's handle going faint, and a macro's and the note source's until they modulate something
    (read off the rendered pixels);
  - the dot on a filter's and a shaper's tab while it sounds out, and the output meter and its clip light (read off
    the rendered pixels);
  - a shaper: switching it on with its tab, sending an operator into it and running it onto an operator and into a
    filter, its page's curve list, link switch and knobs, its Custom curve drawn on the page and in the big editor;
  - dragging the filter response handle, switching between the graph and matrix views, and toggling a matrix cell;
  - picking a filter's slope from its list, a band-pass's 6 and 18 dB refused;
  - making sources unipolar and bipolar from their menus: an envelope's and an LFO's card, a macro's knob and
    handle, the mod wheel and the note source's display;
  - dragging a Param EQ band's handle, and Alt-dragging it for its Q;
  - dragging an envelope's handles, past the display's end too, where the envelope zooms out to stay inside (read
    off the rendered pixels);
  - switching operators and filters on and off with their tabs, the matrix's names and their ghosts in the graph,
    and off from a node's right-click menu, a switched-off one's page dimmed (read off the rendered pixels);
  - the play switches and the glide knob;
  - an LFO starting synced in quarter notes, and its sync switch freeing it;
  - the newer effects' cards (the widener's knobs, the frequency shifter's Shift, the phaser's SYNC, the
    flanger's STEREO, the filter's Key Track and the shaper's and delay's LINK too) and the list of effects with
    their icons;
  - a filter's response moving with the note its key tracking follows (read off the rendered pixels);
  - switching effects with their cards' icons (a switched-off one's staying grey under the mouse, read off the
    rendered pixels), dragging cards by them, and folding cards, the icon staying put;
  - the right-click menu on knobs;
  - the macros' knobs, handles and names, the note source's handle, display and curve editor, and the mod wheel's
    handle (new modulation starting upward);
  - the dashed cards that add effects (opening the list over themselves, staying over a switched-off card's dimming
    at the chain's right end, read off the rendered pixels) and sources;
  - picking the oversampling and the theme, and dragging the glow slider, in the settings dialog (with the editor
    rendered in each theme, and glowing), and closing it with its x or a click around it;
  - undo and redo of drags, wheel runs, node moves, typed values and a removed effect with its modulation;
  - typing values into knobs, header knobs and target depths, with Enter, Escape and a click elsewhere;
  - the preset bar's arrows (asking first while the patch has changed) and the preset browser, which loading a
    preset closes;
  - drawing a Custom wave's partials, at 16 and 64 of them too, the ones a lower count hides kept as they were, and
    the LFO shape editor hiding the inspector;
  - the random LFO's knobs and its dot waiting at the end of a cycle (read off the rendered pixels), the list of
    more waves (Organ among them), the slot list's scrollbar, the shape editor's grid, snapping and RESET, and
    scrolling the source cards by holding a dragged handle at the panel's edge.

  Pass a scale to render larger: `build\release\fmvibe-uishot.exe out 2`.

More checks:

- **clap-validator**: the free-audio conformance test suite, run with `.\build.ps1 -Validate`.
- **In Bitwig Studio**: install the plugin as the [README](README.md#install-in-a-host) describes; Bitwig finds it
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
- Presets are the examples, held in memory. Load, Save and the preset folder show a message instead, since the
  demo has no files. The theme and glow are kept in the browser's storage.
- The computer keyboard plays notes: A W S E D … from middle C, with Z and X to shift the octave.
- WebAssembly can't flush denormals to zero, which `Plugin::process()` does. Instead, the effects' input, the
  voice filters' input and the decimators get a signal far below hearing, behind `__EMSCRIPTEN__`. Without it,
  tails ringing out cost many times the CPU. The plugin's build is unchanged.

### Publishing

`.github/workflows/pages.yml` builds the demo on Linux and publishes it on GitHub Pages
(`https://pcdlin.github.io/FMVibe/`) whenever the `release` branch is pushed. It uses the Emscripten version
`build-web.ps1` pins. Before the first deploy, the repository needs two settings:

- *Settings > Pages > Build and deployment*: Source **GitHub Actions**.
- *Settings > Environments > github-pages > Deployment branches and tags*: add `release`. GitHub creates this
  environment allowing only the default branch, and a deploy from `release` is refused until it is added.

On GitHub's free plan, Pages needs the repository to be public.

## Debugging in a host

Install the debug build (`.\build.ps1 -Config debug -Install`). Bitwig runs plugins in a separate process, so in
Visual Studio use *Debug > Attach to Process* and pick `BitwigPluginHost-X64-AVX2.exe`; breakpoints in `src/` then
hit.

## Layout

| Path | What |
| --- | --- |
| `src/entry.cpp` | Exported `clap_entry` and the plugin factory |
| `src/plugin.cpp` | The CLAP plugin: process loop, events, ports, params, state, remote controls, voice info, gui |
| `src/params.h` | Parameter layout (index ↔ id), slot targets, value mappings |
| `src/params.cpp` | Parameter table (names, ranges, defaults) and value text |
| `src/synth.cpp` | FM voices, matrix, filter routing, LFOs and mod envelopes, telemetry; independent of CLAP |
| `src/wavetable.cpp` | Band-limited operator wavetables, one per octave |
| `src/filter.h` | State-variable and comb filters, plus their response curves for the editor |
| `src/clipcurve.h` | The shapers' curves, anti-aliasing and running, shared by the shaper effect and the voices' shapers |
| `src/analyzer.cpp` | The spectrum analyzer (FFT) behind the Param EQ's, filter effect's and voice filters' displays |
| `src/envelope.h` | AHDSR used by operators and mod envelopes |
| `src/gui/view.cpp` | The editor: layout, drawing, mouse handling. Window-free, so it also renders offscreen |
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
| `.github/workflows/pages.yml` | Publishes the browser demo on GitHub Pages from the `release` branch |

Everything except `entry.cpp`, `plugin.cpp` and `editor.cpp` builds into the `fmvibe-core` static library, which
the plugin and the tools share. The browser demo builds the engine (`FMVIBE_ENGINE_SOURCES`) into its modules
itself.

## Plugin facts

- CLAP id `io.github.pcdlin.fmvibe`. Changing it makes hosts treat it as a different plugin.
- Parameter ids are stored in host projects and automation. `src/params.h` derives them per section with spare
  room: globals 0+, operators 100 + 50·op (their filter sends last), matrix 400 + 10·from + to, filter modulation
  500 + 10·filter + to, shapers 600 + 50·shaper, filters 800 + 20·filter, filter links 900+, effect slots
  1400 + 80·slot (their fields past
  the 80th 12000 + 20·slot), source places
  2800 + 40·place, freeform shapes 4000 + 100·place, the sources' later slots 6000 + 30·place, shaper effects' curves
  7000 + 50·slot, slot bypasses 9000 + 20·place, Custom waves' first 32 partials 10000 + 100·op, macros
  11000 + 40·macro, the note source 11400+ (its slots, then its range and curve), the mod wheel's slots 11500+, the
  Custom waves' partial counts 14000+ and their partials past the 32nd 14100 + 100·op.
  Once released, new fields go at the end of their section's
  enum and fields are never reordered. The same goes for the wave, filter type, effect type and source type lists,
  which are stored by number.
- Like an effect slot, each of the 16 source places has the fields of both an LFO and an envelope, and a Type param
  (Empty, LFO or Envelope) that picks which are used. Hosts name the fields by place ("Mod 3 Rate"), since a name
  can't follow the type; fields of the other type are hidden from them.
- A slot's *target* param stores the target's id + 1 (0 = unused). Targets aren't offered for automation, since
  they are wiring, but they are saved like any other param. Their depths are normal automatable params.
- Edits in the editor reach the host as `PARAM_GESTURE_BEGIN` / `PARAM_VALUE` / `PARAM_GESTURE_END` events. Hosts
  record automation and undo from these, and mark the project as changed.
- The state blob holds `(id, value)` pairs with a version number (currently 1), followed by the editor layout:
  node, filter and shaper positions, selected node, zoom, the routing view, whether the modulation panel is collapsed,
  the order the sources show in, the routes kept at 0 %, the shape editor's grid, the macros' names and which
  effect and source cards are folded. The host's state goes on with
  the settings chunk and a preset chunk (whether the patch changed since its preset, then the preset file's path
  in UTF-8); preset files stop after the layout. Params and layout fields a
  state doesn't have keep their defaults; a layout or settings chunk longer than the loader knows (up to 4096
  floats are read) keeps the fields it knows and is skipped past, so the chunks after it still load; states
  with a newer version are refused. Nothing has been released yet, so there is no compatibility with earlier
  development builds.
- Times and LFO rates are stored as 0–1 on a log curve (1 ms–20 s, 0.02–100 Hz), so host knobs and modulation
  have resolution where it matters. The value text shows ms/s/Hz.
- The plugin takes CLAP note events (preferred) and raw MIDI note on/off, and reports `NOTE_END` so hosts with
  polyphonic modulation (Bitwig) know when a voice has finished. In mono, a note that is taken over keeps its
  host voice while its key is down and reports `NOTE_END` when the key is let go. Per-note modulation follows the
  note that is sounding.
- Voices are allocated when the host activates the plugin, for the current Voices value; the audio thread never
  allocates. Lowering the limit takes effect at once. Raising it past what is allocated calls
  `host->request_restart()`, and the host then deactivates and reactivates the plugin. Each voice takes about
  55 KB at 48 kHz, mostly the two comb filters' delay lines, which grow with the oversampling: at the default
  2x, about 95 KB, so 512 voices use about 50 MB. Voices isn't automatable for the same reason.
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