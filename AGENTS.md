# AGENTS.md

Notes for anyone (human or agent) working on FMVibe, a six-operator FM synthesizer as a CLAP and a VST3 instrument
plugin for Windows and Linux. The README introduces the plugin and says how to build and install it. What the plugin
does and how its editor works, in full, is in [docs/features.md](docs/features.md): read the parts you need before
changing a feature. How to use it, for the plugin's users, is in the user manual, [docs/manual.md](docs/manual.md).

## Ground rules

- Parameter ids, the wave, warp, unison curve and pan, filter type, effect type, source type, amp model, cabinet,
  mic, morph mode and fixed Mult lists, and the state layout are
  stored in host projects and preset files: follow the rules under [Plugin facts](#plugin-facts) when touching them.
  The state version rises at most once between two tagged releases: look at `git diff <last tag> -- src/state.cpp`
  before raising it, and add to the step already made if there is one.
- Keep `ctest` green (the CLAP and VST3 smoke tests, audio quality, UI snapshots). New editor interactions get a
  replay in `tools/uishot.cpp`, new engine behavior a check in `tests/smoke_host.cpp` or `tests/quality.cpp`, and
  what only the VST3 plugin does a check in `tests/vst3_host.cpp`.
- Both plugins sit on one core, `PluginCore` (`src/plugincore.cpp`): the params, the state, the editor's
  `Controller`, presets and the editor window. What one format does goes in its own file (`src/plugin.cpp` for CLAP,
  `src/vst3plugin.cpp` for VST3), and what both do in the core, which tells each host through its `host...()` hooks.
- The plugins build on Windows and Linux: keep both working. A source file goes into both `CMakeLists.txt` and
  `cmake/linux.cmake`, which list their files apart. A change to code under `#ifdef _WIN32` (files, settings, dialogs,
  windows, the host's main thread) needs its Linux branch too, and the other way round, and what the editor's window
  does goes into both `src/gui/editor.cpp` and `src/gui/editor_x11.cpp`. Neither build compiles the other's files, so
  say in the commit which platform a change wasn't built on: `build.ps1 -Validate` on Windows and `./build.sh` on
  Linux are the checks (see [Linux development build](#linux-development-build)). Where a feature behaves otherwise
  on Linux, the "On Linux" section of [docs/features.md](docs/features.md) and the manual say so.
- When a feature changes, update [docs/features.md](docs/features.md), and the user manual,
  [docs/manual.md](docs/manual.md), where it says how to use what changed: the manual is for the plugin's users, so
  it says what a control does and how to work it, not how the engine does it, in short, plain sentences. Keep
  `README.md` to its short description and the build and install instructions.
- Don't start Bitwig or another host yourself: the plugin is tried in the host by hand.
- The editor draws through `Canvas`, which has three backends: Direct2D (`src/gui/canvas.cpp`), the browser
  demo's 2D context (`src/gui/canvas_web.cpp` with `web/site/canvas2d.js`) and Cairo for the Linux development
  build (`src/gui/canvas_cairo.cpp`). A new drawing call needs all three.
- The editor's corners and text sizes keep to the scales in `src/gui/theme.h`: a radius is `kRadiusMark`,
  `kRadiusControl`, `kRadiusCard` or `kRadiusPanel` (a ring around something takes its radius plus how far out it is),
  and a text size one of the `kText...` sizes. New drawing uses them rather than literals.
- A button looks as what it does (the list above `paintActionFace()` in `src/gui/theme.h`): an action is an icon on
  `paintActionFace()`, a param's switch `View::paintButtonFace()`, a button that opens a list its name and
  `paintChevron()`. A new button takes its role's painter rather than drawing a face of its own.
- Text the plugin shows (labels, tooltips, menus, messages, parameter names, remote control pages) is worded in
  `src/uistrings.inc` and looked up by name: `tr(Str::Name)`, or `trf()` for a wording with `{1}`, `{2}` in it. New
  text gets a line there. Tests that match on text (menu entries, page names) look it up the same way, so a
  rewording keeps them green.

## Build

What you need, `build.ps1` (which runs the tests below with every build, unless `-SkipTests` builds only the plugins),
the plain CMake commands and installing into a host are in the [README](README.md#build).

## Testing

`ctest` runs five programs, four at a time (the test presets' and `build.sh`'s jobs):

- **Smoke test** (`tests/smoke_host.cpp`): a small headless host. (It and the VST3 test set `FMVIBE_SETTINGS_DIR`,
  which `src/usersettings.cpp` reads in place of the roaming app data, so their editor resizes don't land in your
  settings file.) It first runs itself as other processes sharing a settings file: categories, favorites and recent
  presets edited in two at once all counting, the author another typed last kept through this one's edits, four editing at once losing no edit, and a file another program holds
  waited for or, held longer, left as it is with the edit written later. It sets `FMVIBE_FIXED_SEED`, as the VST3 test
  and `fmvibe-bench` do, for instances whose notes draw the same randomness each time (the plugin otherwise draws its
  own for each instance). It loads the `.clap`, creates instances on two threads at once, checks that two instances'
  noise differs without that variable, that an instance run out of memory (a process of its own, limited by a job
  object) tells the host, plays silence and activates once there is room, that Enter in one of two editors in one host
  window leaves a question open in the other, and checks the parameter
  table, text round-trips (at each end and just inside it too, the slowest sweep rate and the shortest time typed
  neither still nor Off), the note source's range (every MIDI note, on to the pitch at 20 kHz) and remote-control pages. It then plays
  notes, checking:
  - hosts given the params they automate and see and the slots' depths and Amounts, and not the effects, the
    sources' own fields, the slots' wiring or the editor's drawn shapes (whose values still read back by id); no
    effect getting a remote-control page, a page per source, and the pages pointing only at params hosts are given;
  - sound comes out and `NOTE_END` arrives after the release;
  - voice stealing and choke, and a reset ending the notes playing, their ends reported in the next block;
  - per-voice host modulation, and a mod-envelope slot driving its target, and running its release out after
    the operators have faded (the voice reporting `NOTE_END` only then); likewise a Curve in Sustain, let go in its
    loop, playing on to its curve's end (`NOTE_END` half a second on, once; at once with nothing to modulate), and
    looping on under the sustain pedal until it comes up; and an operator's drawn envelope, let go, playing on to its
    shape's end, its voice's `NOTE_END` coming once then (at once on its AHDSR);
  - spectrum checks on an A4: the matrix produces FM sidebands; a Ring route at 100 % leaves only the sum and
    difference and at 50 % is AM, Linear FM keeps the mean pitch (through zero too) where Exp FM lifts it by its
    octaves, and feedback stays Phase whatever its mode param says; every wave sounds; a saw's 2nd harmonic sits at
    half the fundamental, and Alt Sine's, Organ's and Vowel's where their series put them; a Custom wave plays its
    partials, only as many as its count (the 40th silent at 32, heard at 64); a sine synced at 2.5 times an A4
    playing the A4's harmonics and not its own 1100 Hz, and Squeeze moving a square's pulse width (its 2nd harmonic
    from none at 50 % to about 0.63 of its fundamental either way at ±50 %), and the Sync warp at 3 times a cycle
    playing an A4's 3rd harmonic alone, at 3.5 times its harmonics, not 1540 Hz, and at 16 times its 16th alone; a low-pass at 300 Hz and a high-pass
    at 2 kHz move that ratio the right way (the low-pass's Mix at 0 leaving the saw as it was, at 50 % between), each slope from 6 to 24 dB harder than the one before (the 6 dB
    low-pass a one-pole), and a band-pass set to 6 or 18 dB plays 12 or 24;
  - the ladder's slopes tapping one to four of its one-pole stages, its high-pass's one to four one-pole high-passes,
    and its band-pass two poles at 12 dB and two of them at 24 (6 and 18 playing 12 and 24), those two played at a
    quarter of an operator's level, under their stages' saturation; the ladder (its low-,
    high- and band-pass), the diode and both Sallen-Key's, at full resonance and plucked, ringing on at their cutoff, steady and finite (silent by then at 80 %); an A4 sine
    under the ladder's cutoff coming out with a 3rd harmonic from its saturation, more with Drive, where the
    low-pass's stays clean, and its resonance thinning the note, where the low-pass's leaves it as it was; the diode
    playing 24 dB whatever its slope says, the Sallen-Key low-pass two poles at its cutoff and its high-pass lifting
    a saw's 2nd harmonic; the Formant type putting noise through a vowel's formants (A's first at 600 Hz, I's at 250);
    the Morph type playing exactly the low-pass, the band-pass or notch and the high-pass at its ends and middle, at
    12 and 24 dB, between them along the way, and set to 6 or 18 dB playing 12 or 24; and the low-pass (12 and 24 dB), the ladder, the diode and the Sallen-Key low-pass each
    sounding unlike the others, by at least 3 dB in a saw's harmonics through them, the 3rd harmonic they give a sine or
    how far the note falls with resonance (the figures printed);
  - every filter type stays finite at high resonance at every slope, and a voice ringing through a resonant comb
    or a self-oscillating ladder, diode or Sallen-Key still ends;
  - an operator's Ratio taking modulation on its log curve, continuously (a seventh of its range an octave), and one
    set to 64 modulated on to 128 and no further;
  - a fixed operator playing its Freq × Mult (at ×0.1 and ×10 too) for a C3 and an A4, under a bend, with its
    Pitch and Detune turned and modulated;
  - a new instance has only operator A on, no filter key tracking or going out, its shaper effects linked, its
    frequency shifters' range at 100 Hz, its flangers stereo and its Custom waves at 32 partials; a switched-off
    operator is silent, and a switched-off filter passes nothing; a filter routed nowhere isn't heard, and goes out
    by its Out; filter 1 feeds filter 2;
  - a shaper's output modulates an operator (and one that is off doesn't), a hard shaper into a filter squares a
    sine off, and a shaper routed nowhere isn't heard but goes straight out by its To Out; a filter into a hard
    shaper squares it off too, and a filter and a shaper feeding each other in a loop still end with their note,
    and stay finite when the loop lifts itself past unity; a hard shaper feeding only another shaper squares a sine
    off through it, a shaper fed back into itself drives itself into its curve, and shapers feeding themselves and
    each other past unity stay finite and end with their note; a filter feeds an earlier filter, one fed back into
    itself grows louder, and filters feeding themselves and each other past unity stay finite and end with their
    note;
  - the mod wheel moves nothing until it is assigned, and then its target, like a macro; and a hardware mod wheel
    (MIDI CC 1) moving the Mod Wheel param, and its target with it, the host told of the param's value;
  - the pitch bend source moving its target: bipolar, nothing at rest and half its depth bent fully up; unipolar,
    half at rest and all of it fully up; and the Pitch Bend param bending an A4 two semitones up, the source
    following it set and modulated by the host, and a host's modulation of it for one note bending that note alone;
  - a MIDI source following its controller (and not another), on pressure the channel's and a note's own
    (polyphonic aftertouch or CLAP's pressure expression, not another key's), and on slide resting in the middle and
    following a note's brightness expression; Slew taking a controller turned up mid-note slowly, and a Random source
    standing still (at 0 Hz) giving each note a level of its own;
  - MPE over MIDI, switched through the state's settings chunk (which saves it): with it on, a member channel's
    pitch bend bending its note alone and the master channel's every
    note, and a member channel's pressure being its note's alone (the master channel's standing in until it sends
    some), in mono too for a note taking the voice over; with it off, any channel's bend bending every note;
  - with host sync off through the state's settings chunk (which saves it, a new instance following the host), a
    synced LFO playing at the synth's own tempo whatever the host's, and an anchored one counting the host's time in
    beats of its own tempo;
  - a synced LFO's note value taking a macro's and the host's modulation in steps, a part of a step leaving it as
    it is;
  - an LFO's Phase moving where notes start it (a one-shot running from there to its cycle's end), while a free
    one keeps its clock, and an anchored one stands where the host's playing transport puts it;
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
  - the FX order changing the result, and a slot past the 16th playing at its place in the chain (the slots holding
    an effect wired one after another, as the editor wires a chain);
  - the effects graph: its input straight out, two wires at half as one at full, and a split's two bands (at 6, 12
    and 24 dB) and three (at 12 and 24) summing back flat, each slope parting its bands as steeply as it says (three
    bands set to 6 playing at 12); a join's solos letting only the band soloed through (both when
    neither or both are), a three-band split's middle one too; a hard shaper on a split's low band squaring an A4 only when the A4 is in that band; the
    chain's retired places doing nothing; a loop, which only params make, staying finite; and a macro taking a wire's
    level down;
  - each effect on its own, among them a chorus at 0 % Mix leaving an A4 alone, its Spread panning its voices
    apart (none keeping them in the middle) and its bass going past its voices (an A1's sides far narrower than an
    A4's); a shimmer's tail rising in octaves
    where a hall's has none (a fifth up or
    an octave down as its Pitch says, and nothing shifted at 0 % Shimmer), the widener
    widening the sides above its high-pass while the mono sum stays dry, the phaser's and flanger's notches deepest
    at 100 % mix (half wet), the flanger's taking half of the tone in it away at 50 %, a stereo flanger's sides
    sweeping apart and a mono one's together, a linked shaper keeping its level, a symmetric one shaping its
    lower half as its upper one (a drawn curve losing its 2nd harmonic, Asymmetric left as it is), and
    the filter effect's key tracking putting a cutoff at C4 onto an A4, the frequency shifter moving by its
    Shift's share of its Range, and the Param EQ's third band doing nothing off and playing once on, a band switched
    off left out, its notch taking its frequency away (and leaving an octave off it mostly alone), and its Amount
    mixing it with what goes in; and the utility's Gain going from silence up to +12 dB (0 dB to begin with, -6 dB at its middle), its
    Pan turning the other side down, its Width scaling the sides and keeping the middle, and its Mono Bass making a
    low note's sides mono and leaving a high one's;
  - unison copies spread across the stereo field (and match when centered);
  - the sequencer in Host playing its note with the host's transport, no key held, processing kept awake and no
    NOTE_END for a note of its own; and in Key, a key held playing the pattern and its NOTE_END coming as it is let
    go;
  - an operator's Pan placing it, equal-power, through a filter too, and a modulator's leaving what it modulates
    alone; and keyboard scaling set by the host halving an A4's level an octave over its breakpoint, linear and
    exponential, the breakpoint moved with no depth changing nothing;
  - mono: a new key takes the voice over, letting go returns to the held key, and every note reports `NOTE_END`
    exactly once;
  - the sustain pedal (CC 64) holding a note let go until it comes up; a key struck again under it letting its held
    note go, and a key still down sounding on after it comes up; in mono a key let go returning to the key still
    down, else the pedal holding the note, each note ending once; poly legato joining a note the pedal holds; All
    Notes Off (CC 123) releasing every note (the pedal holding them), and All Sound Off (CC 120) ending every note
    at once and a reverb's tail with them;
  - legato keeps the envelope at its sustain level while retrigger restarts the attack, in mono and in poly, where
    a note played over a held one joins that one's envelopes (at its sustain, or swelling along with it); glide
    slides between
    overlapping notes and skips detached ones unless Always is on (checked by tracking the pitch);
  - a lower voice limit applies immediately, while a higher one, stepped up a block at a time, requests one
    restart once it has settled (the host's timer-support ticking; again once lowered and raised before the host
    restarts, none if lowered again before it settles, and from the main-thread callback for a host without timers)
    and is reported through voice-info after reactivation, and a state with a higher one, loaded while active,
    requests one at once;
  - the latency extension reporting the voices' decimator's couple of samples, and about 5 more with a shaper effect
    in the line, a state that moves it, loaded while active, asking for a restart, and the reactivation telling the
    host's latency extension; a note held as the state loads holding that restart off until its key is let go
    (asked for then, from the main-thread callback) and playing on through it in its release, with no new attack,
    its `NOTE_END` coming once, where a key struck after the restart was asked for and still down as it comes ends
    its note, as does an activate the plugin didn't ask for, reporting each one's end.

  It opens the editor inside a hidden host window, where the preset bar's next arrow loads a preset with no param
  events, the host asked to rescan the values and marking the project changed, and drags the Volume knob with mouse
  messages. It checks that the param changes and that the host receives gesture-begin, value and gesture-end events, and that an older
  value the host hands back right after isn't taken; that a shaper effect dragged along the line, and that undone, ask
  the host for no restart while one taken out of the line asks for one, from the main-thread callback; that the Voices
  knob dragged past the voices allocated asks for none while the mouse holds it and one once let go and settled; that on the SEQ tab a wheel message with Ctrl in its keys zooms
  the roll, though the thread's own idea of Ctrl is up (as while the host has the keyboard); that with nothing
  sounding processing sleeps, but not while the
  editor shows a free LFO running (the plugin asking the host to process again as one comes on); that a new editor,
  and one at a saved zoom 3, opens within the screen's work area, and folding the keyboard asks for sizes within it;
  and that an editor
  sized at the system's DPI and given a window
  at another (one not DPI aware) asks the host for the size at the window's, its zoom kept (at the same DPI, for
  nothing). It saves/loads state
  through a stream that only moves 5 bytes per call, a version 1 state's Volume loading 12 dB lower, a version 2
  state's Legato loading off in poly (and as it was in mono), envelope attacks, decays and releases typed down
  to 0 ms through fractions of a millisecond, a stored 0 loading as 1 ms and a version 3 state's depths on them
  loading scaled by as much as their range grew, a version 4 state's ratio loading from its Coarse and
  Fine, their modulations moved onto Ratio (a version 5 state's Coarse and Fine doing nothing), envelope times
  stored past 10 s loading as 10 s, a version 6 state's Glide of 20 s loading as 6 s and its depth on Glide scaled
  by as much as the range shrank, a version 7 state's chain order loading as the effects graph's line (a version
  8 state's retired one doing nothing, its places putting the effects in order), a layout as large as one gets among them (and one of NaNs,
  which loads as the defaults), a slot's aux keeping its source by the number it is stored by (a version 15 state's
  Macro 1 and operator A, and the 17th place, each saved back as it was), and checks that
  every remote control page has an id of its own, a source in the last place given its page. It also reports CPU cost: on the development machine, 16 voices × 6
  operators take about 13 % of one core at the default 2x oversampling and about 30 % with every operator at 8
  unison copies (the smoke test prints the figures). Time a change to the voices' loop in this build, with one copy
  and with eight, against the commit before (`tools\bench.ps1`, under More checks below): a Linux measurement doesn't
  carry over to MSVC's code, where a
  `std::copysign` that GCC inlines became a library call on every wavetable read (a third more CPU at eight copies),
  and envelopes run a block ahead into a buffer cost a sixth more with one copy than stepping them in the loop.
- **Audio quality** (`tests/quality.cpp`): drives the engine directly and measures, at every oversampling factor:
  - the decimator's passband ripple and alias rejection;
  - aliasing (the energy off the harmonics, 20 Hz–20 kHz) of a high saw, bright FM (phase, linear and ring),
    feedback, a driven filter and a driven, resonant ladder (its low-, high- and band-pass), diode and Sallen-Key;
  - zipper noise from an LFO on an operator's level and Pan, a matrix amount, a filter cutoff, a resonant ladder's
    cutoff, a formant's vowel, a morph's place and a wire of the effects graph; and on the voice filters (a resonant
    low-pass's and a quiet ladder's cutoff, a formant's vowel and a morph's place, each within one stretch of its curve,
    and the low-pass's cutoff again with a silent operator on its Resonance, which works the filter out every sample)
    and the effects' frequencies (a phase disperser's Freq, a filter effect's cutoff, formant vowel and morph place, a
    Param EQ band's frequency, a phaser's Center and a widener's Cutoff), nothing but the sine's own sidebands at 7 Hz
    apart coming out, where control-block steps would put energy near the control rate's multiples (held still in
    brackets);
  - clicks when a voice is stolen (and six at once by a chord), when a note joins a held one in poly legato
    (fading in over 5 ms, its Random LFOs carrying on with the held one's step), and when a param (Volume, and an effect's: a compressor's Makeup, a delay's and a
    reverb's Mix, a DJ EQ band, a three-band dynamics' Output, an amp's Output, Mix and Gain, a utility's Gain (0 to
    +12 dB), Pan and Width, a wire's Level) jumps while events split the block into single frames (and a delay's Mix
    and a wire's Level ramping no faster at 96 and 192 kHz than at 48),
    when a Param EQ band is switched on and off, when a utility's Mono Bass is turned on from Off and off again, when
    a phase disperser's Stages changes under a sine (fading from one count to the other, in single frames too), and
    when a wire of the effects graph is connected and taken away;
  - editing the effects graph as a note plays, neither clicking nor dipping (each period's peak kept within 10 %):
    an effect added at the line's end and one taken out, a split taken out with its join, a split's slope
    changed, and a split going from two bands to three and back at 12 and 24 dB, the note in its middle band; and two effects swapped along the line, ducking out and back in without a click;
  - an effect in the graph that the output doesn't hear not running (its port silent, nothing ringing on after it),
    and running once the output reads it; and an effect taken out of a loop the params make (its wires left as a
    host or a state can) leaving the output playing what is left, steadily;
  - the effects graph's ports telling the editor how loud each is: IN and an effect on the line at the sine's level,
    an effect not in the graph silent, and a second of silence taking them about 90 dB down; and nothing measured
    while nothing analyzes (no editor open);
  - the amp: every model at its middle knobs about as loud as a sine at -12 dBFS going in (the cabinet on), the
    energy off the fundamental growing with Gain (Heavy's more than Clean's), Heavy's aliasing at full Gain and
    Volume on a C7 falling as the oversampling rises (35 dB down at 4x) and the cost of each, Treble and Bass tilting
    the tone their ways, the cabinet taking the top away and the mic at the cone's edge darker than at its center,
    one amp on both sides' sum in mono and one a side in stereo, Mix at 0 leaving the sound alone (but as late as its oversampling), the Fuzz putting out
    no DC, the mic a metre back hearing the room's last reflection, and the model, cabinet, mic, cabinet switch and
    oversampling changed under a note crossfading rather than clicking;
  - the three-band dynamics on a sine in its mid band: a quiet one lifted and a loud one pulled down as its
    thresholds and ratios say (the band telling the editor its level and gain), the lift capped by Range, peaks
    followed with RMS off, 1:1 both ways leaving the level alone, ∞:1 holding a loud one at Above (its text
    reading back), and linked bands still playing their own settings (Link being the editor's); a note out of
    silence not lifted by the Range the silence had, and OTT's settings following the curves the OTT preset was measured
    to have (sines in each band at four levels), pulling a loud saw down and lifting a quiet one as it does, and
    starting neither at the lift the silence had; the band's meter not shooting up as a loud note gives way to a
    quiet tail that the band lifts; and a
    version 5 state's retired three-band compressor loading as a three-band dynamics doing what it did, its
    modulation along (a linked one's bands taking the low band's settings), while a version 6 state loads one back
    as saved; and a version 9 state's depths on its thresholds loading three quarters as deep, as their range grew
    from 60 dB to 80, a version 10 state's as saved; and a version 10 state's Random source loading as one standing
    still at 0 Hz, its LFO with the retired Random shape as a Random source running as it ran and an LFO at Rate 0
    (its slowest then) playing as slowly rather than standing still, a version 11 state's as saved; and a version 2
    state's depths on envelope times and holds loading as far as they swung (their range 0 to 1 then), a version 3
    state's on the times alone, a version 4 state's as saved; and a version 11 state's LFO with the retired Custom shape loading as a Curve source running as
    it ran (its points and polarity kept), a version 12 state's as saved; and a version 12 state's depth on an
    operator's Unison Detune loading half as deep, as its range grew from 100 cents to 200, a version 13 state's as
    saved, and its Unison Pan loading Ramp, as its copies played, where a version 13 state without one loads
    Alternate, the pan a new operator starts with, and its depth on a source's Division loading 26/28 as deep, as it
    grew two note values slower, a version 13 state's as saved; and a version 13 state's sequencer curves loading with
    points that hold their ends as they played, a version 14 state's as saved, wrapping around; and a version 14
    state's Sync warp's amount and the depth on it loading 7/15 as large, as its reach grew from 8 times a cycle to
    16, a Squeeze's as saved, a version 15 state's as saved; and a version 16 state in Key with its Root off C4 loading
    that difference into Transpose (and Octave past its reach), one in Host as saved, a version 17 state's as saved, and one without the sequencer's Play loading Host, a version 17 one Key;
  - the phase disperser's Key Track and Stretch against the chain they make, from the same coefficients: an impulse
    through Key Track with a C4, a C5 and (at 50 %) a C6 coming out as through the chain at Freq and an octave up, and
    through Stretch 4, 37 and 4 at 96 kHz (eight samples) as the plain chain's impulse response spread that far apart,
    silence between; Stretch 32 at 400 Hz peaking at 400 Hz, 1.1 and 1.9 kHz; Stretch changed under a sine fading out
    and back in, its third difference far under an instant switch's, in single frames too; and 64 stages at full
    Pinch and Stretch, tracking a C7, staying finite and ringing out;
  - the chorus's Voices (2, 6, 3, 2) and Spread (0, 100, 0 %) changed under a sine, all wet, gliding rather than
    clicking, and its Delay (3, 25, 8 ms) and Depth (0, 100, 20 %) jumping crossfading rather than clicking, in
    single frames too, as the flanger's Delay (1, 10, 3 ms, through zero too) and Depth (0, 100, 30 %) do: no sample
    stepping further than the sine's own do; held still, an impulse coming out once a voice, at
    its place along Delay and as loud as 1 / sqrt(voices) (2 and 6); and swept, its voices together playing a sine at
    440 Hz and 5 kHz about as loud as it went in, with 2, 3, 4 and 6; and its Feedback full up either way staying
    steady as Voices drops from 6 to 2 under a loud sine, then dying away;
  - the reverb's tails: every algorithm's late spectrum without lasting resonances and its two sides unalike, an
    undamped tail falling 60 dB around 1 kHz in its decay time (and around 8 kHz in nearly as long, its lines' reads
    between samples taking nothing from its top), and a reverb with a 200 ms pre-delay ringing on
    until its tail has been heard, and stopping after; a sample that isn't a number, and one louder than any sound,
    going into a delay and a reverb leaving them sane, ringing out and playing on; a delay switched off and on again playing back nothing it
    held, its lines part written or written all round, and its times then turned to their longest and shortest (as a
    reverb at its largest and longest pre-delay plays back nothing it held), though its lines are cleared only as they
    are read; a compressor running its release out in silence, a quiet
    sine after it untouched; a comb as long as its line ringing on between its pulses and, once rung out, bringing
    nothing back; and effects that have rung out costing little for silence, which comes out as silence, also while
    the editor analyzes them (an analyzer's spectrum falling away through it as it would);
  - a shimmer's tail after an A4, with Shimmer half and full up: falling 60 dB in about its decay time, its energy
    on the octaves and the first two octaves up narrow lines (not smeared by the shifter's crossfades); and at the
    longest decay, undamped and full up, still dying away; and Shimmer turned down to 0 in short chunks and up again
    once the reverb has rung out bringing back nothing of the tail before;
  - a shaper effect switched off, its drawn curve changed and on again playing the new curve from its start;
  - the Stack curve: a stack of three playing about as three linked shapers of one stage in a row (far nearer
    them than one stage), one stage passing a quiet sine at its level and saturating a loud tone unevenly, a stack of
    eight passing quiet sines from 1 to 15 kHz within a dB (no low-pass, the top lifted back), a stack of
    eight at full drive ringing on past a note longer than one stage does and then stopping (the effects too), and
    thumping far more between 10 and 60 Hz after the note, the stack turned up and down under a note in single frames
    staying finite and within the curve's reach, a shaper effect switched from Soft to Stack and back under a quiet
    sine crossfading rather than clicking (no sample stepping further than the sine's own do), and a voice's shaper
    stacked eight high ending with its voice;
  - a drawn shaper curve's points moved gliding in rather than clicking, in whole blocks and in single frames, and
    dragged up and down putting out no DC, made symmetric too;
  - a quiet, slow sine through a drawn shaper curve coming out as drawn (a few samples late, for the shaper's twice
    the rate), without spikes where it crosses the curve's table steps; a symmetric drawn curve that doesn't pass through the middle putting out silence for
    silence, and an input that isn't a number reading inside its table;
  - the filter effect switched to another type or slope and back (every newer type among them, and the analog
    types, which share their stages, from one another and from a morph), and a plate switched to a hall and back,
    playing nothing their stages held before; and the effects running on until a shaper's DC blocker has settled;
  - the phaser's and flanger's polarity: negative moving their notches to where the positive peaks were, and
    turned over mid-sine fading through nothing rather than clicking;
  - a frequency shifter's mirror image 60 dB under the shifted note from 50 Hz to 10 kHz, at 44.1, 48, 96 and
    192 kHz; what it takes past 0 Hz or half the rate 60 dB down rather than reflected back, what stays between them
    at its level, and a spiral of its feedback running out at 0 Hz;
  - the effects keeping the top where their knobs don't take it: a quiet 15 kHz sine through a soft shaper and a
    driven high-pass filter effect within 1.5 dB of a 1 kHz one, the parametric EQ's bands up high (a bell, a dip, a
    high shelf, a low cut and a high cut) sounding within 1.25 dB of their drawn curves from 3 to 15 kHz, and a
    delay's sixth echo of a 10 kHz burst, at a time between two samples, within 0.5 dB of Feedback's fifth power
    under its first;
  - Drive into the ladder and the diode, in a voice filter and the filter effect: a full-level saw through 85 %
    resonance coming out of full Drive with its peak some 20 dB further under its fundamental than undriven (with
    Drive held under 1, as it was, 9 to 10 dB in a voice filter and 14 in the effect, which hears the voices 12 dB
    down);
  - Drive keeping a sound's level: a saw at -24 to +6 dB through an open low-pass and Sallen-Key low-pass, at Drive
    50 and 100 %, coming out within 1 dB of its level undriven, and a note starting at full Drive peaking no more than
    2 dB over where it settles;
  - the Random source's steps and glides, and at 0 Hz a value for each note, anywhere, at once and held (a mono
    retrigger taking a new one), a freeform shape's points at one time (a vertical line, its last
    counting, and a point at the cycle's end holding a one-shot there), a Curve source playing sample for sample as
    an LFO with the retired Custom shape did, the phaser's and flanger's sweeps synced to the host tempo, the parametric
    EQ's, the phase disperser's and the voice filters' analyzers (falling away as fast at 96 kHz as at 48), and that each example preset holds valid values
    and sounds at a sane level, its triads at full velocity (a drum's single hits) peaking under 0 dBFS;
  - each factory preset's file being sparse, as the preset tool writes it, and each saying it is FMVibe's, in a
    built-in category, with a line of 80 characters at most; and a name from up to 0.11 ("LD Screamer") naming the
    preset as it is named now;
  - a patch using the places past the 16th (the last's LFO, its shape and slot curve, an aux between the 17th place
    and a macro) loading back as saved, its auxes stored by the numbers sources are stored by, and the layout keeping
    the later places' order and folds and an empty modulation's source;
  - a preset's author, category and description reading back as written (UTF-8 too, from a sparse patch and a full
    one, and as the library's scan reads them past the params), as none from a layout before them, and nothing from a
    stream that isn't a state;
  - the tempo a preset was saved at reading back as written, and as none below 20 BPM or from a layout before it;
  - the mod wheel leaving the FM alone until it is assigned, and the output meter's peaks, fall and clip light;
  - the DC blocker before the effects: a squeezed square coming out with no DC, a Ring route at unison going into
    a hard shaper as its octave alone (clipped evenly, no 4th harmonic), and the output exactly silent once the
    note has rung out, the effects resting;
  - an operator's output as a source: one at 5 Hz swinging its target both ways, one at 12 kHz moving it every
    sample rather than averaging away, unipolar taking its target one way, silent or switched off moving nothing, and
    silent as an aux scaling its modulation to nothing;
  - audio-rate modulation: an operator at 3 kHz (two whole cycles a control block) putting its sidebands around an A4
    through each kind of param a voice plays by (an operator's Out, Ratio, Pitch, Detune, Freq, Phase, Pan, sustain,
    Unison Detune, Spread and Blend, Warp Amount and send, a route's depth, FM Amount, the voice's Pitch and Pan, a
    filter's Resonance, Drive, Morph and Out and a shaper's Drive and Output), and none without it; one swinging a
    Ratio an octave at 7 kHz aliasing 40 dB down at 2x; and one on its own pitch, level and phase, and one on many
    params at once at full depth, staying finite and ending with its note; and filter FM carrying on once an operator
    on the filter's Resonance stops (its depth taken to 0), and a key-tracked filter following the voice's Pitch as an
    operator moves it (an operator fixed at 440 Hz through it taking on its sidebands); a voice filter switched off and
    on again mid-note, its cutoff moved meanwhile, starting at its settings as they are then;
  - filter FM, an operator on the Cutoff of a voice's filter and of the filter effect: sidebands from a modulator at
    3 kHz (two whole cycles a control block), every filter type finite at full depth and resonance in both places and
    a voice ending with its note, the aliasing at each oversampling factor, a slow operator moving a cutoff as far as an
    LFO does, and the filter effect's FM taken over by a new note without a click;
  - a macro's knob modulated: along a chain of macros out of their numbers' order (the telemetry saying where the
    knobs stand), in a loop a state holds, steadily and alike each time, and per voice by velocity, a loud note's
    knob silencing it while a soft one played with it plays as it would alone;
  - where the pitch bend stands, as the bend wheel shows it: the host's MIDI bend and modulation on top of the Pitch
    Bend param as set, with no note and in a note with its own modulation; and Bend Range taking none, from a source
    (a slot a state from before holds) or a host;
  - Volume setting the level after the effects, and a mod envelope's level half way through its release;
  - an LFO on an effect, retriggered or free, still modulating it through a reverb's tail once the note's voice has
    ended, the editor told where it takes the effect and where the LFO is as it moves on, and stopping once the
    effects have rung out;
  - every note reporting its end once: in mono a note taken over in its release, notes held in poly as mono turns
    on (a key let go not moving its voice onto a key another voice plays), with note ids a note on a key
    struck again while it rings (without them, the key staying down as the last note ends), the voices a lower voice limit cuts, a note with an id while its key sounds on
    without one, mono's all notes off (releasing on the key it plays), a reset and forty notes at once on one voice;
    a chord into voices that have all played taking the oldest, every note of it sounding (and a slow attack's new
    note not stolen first); a voice filter whose send goes to 0 ringing out, and a plucked comb ringing on through
    the gaps between its pulses once let go, then ending; a velocity or note expression that isn't a number staying
    finite; host modulation of a macro, the mod wheel, the note source's depth and velocity's
    moving their targets; a comb never reading past its line; a voice's right channel coming back from mono where
    the left is;
    an envelope's curve bent again after a straight stretch carrying on along it; and a 0 ms attack, decay and
    release taking a sample each, a 0.5 ms and a 1 ms attack their time, a sustain change after a 0 ms decay still
    smoothed, and a voice with a 0 ms attack at full level from its first sample;
  - a Custom wave playing the partials drawn, taken up as it is picked and changed partial by partial as it plays;
  - an operator's Phase starting its wave that far into its cycle, set or modulated; turned under a held note (a
    synced one too) playing on as a note started there, without a click, 100 % leaving it as at 0 %, a new note
    starting where the knob stands, and an LFO on it swinging a held note's phase as far both ways, from a knob at 0 %
    too, without zipper;
  - an operator's Note Sync and warps: the aliasing of synced sines and saws and of every warp (Bend, Squeeze,
    Pulse, Steps, Sync, either way, and synced and warped together) at each oversampling factor, every warp at 0 the
    plain wave bit for bit, sync switched on and off and a warp switched on and changed to another under a note
    (in single frames too) no rougher than the sound before, after or the plain wave between, and an LFO on
    Squeeze's amount and on a synced operator's ratio leaving only the note's harmonics and their sidebands;
  - an anchored LFO standing where the transport puts it, from its Phase, by beats when synced and by seconds when
    not (a one-shot not anchored), synced down to 8/1 (a cycle every eight bars, 4/1D's number as it was),
    following it through a block split into pieces, running on from there while the
    transport is stopped, and a Random source anchored running free and one-shot retriggering;
  - a Curve's loop region and Sustain: with its loop the whole curve, a retriggered one's phase and a free one's
    shared clock moving block by block as `phase - floor(phase)` moved them, to the bit; Retrigger playing from its
    Phase into its loop and then round the loop alone at the speed it is drawn, Free cycling the loop alone, Anchor
    standing on the loop's cycles from the song's start (synced and not), One-Shot playing the whole curve whatever
    its loop, Sustain winning over the other switches (an LFO having none) and the loop's ends no modulation target;
    Sustain looping while the key is down, let go leaving the loop at once from the level it was at (no step past the
    curve's own) and sliding onto the next point, on to the curve's end, its voice running on until then, the pedal
    holding it in its loop, a loop to the curve's end gliding there over 5 ms, one ending on a drop going down with it
    over as long and following its curve after, in mono a new key in its release
    starting it again and legato keys looping on; a loop of no length holding, and one the other way round, outside
    the curve or not a number playing in order or as the whole curve;
  - an operator's drawn envelope: playing its shape sample for sample, bends and all, and holding its end while held;
    let go at its end fading out over 5 ms; in Sustain looping its loop while held and, let go, leaving it from the
    level it was at and sliding onto its shape by its end, a loop ending on a drop going down with it over 5 ms;
    started again going on from the level it was at; a 5 ms Slew smoothing a jump and its loop's wrap and, let go,
    running down to nothing within its slew's time after its fade and standing idle then; counting as rising until its
    shape's top; an AHDSR releasing running on when its operator goes back to a drawn envelope that is done; and in the synth, an operator's level and place following its
    shape, its voice ending once it has faded out, and in Sustain looping while held and playing on to its end; and a
    Curve source's Slew lagging a square's step;
  - the scope capturing nothing until asked, then an operator's cycles from where they start (a sine's a sine, a
    square's a square), standing still from one span to the next under FM, two cycles differing under a modulator
    at half the ratio, a slow span shown as it fills, nothing again once it isn't asked for, a low note's (an A0's)
    standing still while held and, let go of, blending each span into the next, moving at every snapshot in small
    steps, and a high operator's (a cycle of 4.5 samples) for little more than the voice costs;
  - the header's output scope capturing nothing until the synth analyzes for the editor, then two periods of the
    note, standing still from one span to the next: a sine, a wave crossing 0 many times a period, one repeating
    every other period and a high note's over more periods; a low note's blending each span into the next, as the
    operator scope's does; falling silent with the output, and nothing once switched off;
  - a host's automation and modulation splitting blocks into pieces as they move params (some chosen, the rest any
    param that can be modulated, wiring too): the synth told param by param which changed playing sample for sample
    as one working everything out again for every piece;
  - the unison modes: Linear and Ramp spreading the copies evenly, to the bit, as unison always did; Center
    bunching them about the pitch, Supersaw spacing seven as a supersaw does, Random putting one in each stretch of
    the detune (the same through a note, new for another), Alternate putting copies next in pitch on opposite sides
    and Random pan dealing them the even places; and on an A4, Center's inner copies nearer the pitch than Linear's,
    Alternate's lowest copy on the other side from Ramp's, and full Blend taking the outermost copy 20 dB under the
    middle one, in what a modulator's carrier plays too;
  - the sequencer: its notes on their steps with the host's transport, the bar round again and alike whatever the
    stretches it plans, each note once where stretches start on its step with the host's position a hair early or
    late there (as a position in seconds rounds), the transport starting in a note's middle playing the rest of it
    once (not one its chance leaves out), a note put down under the playhead playing at once, and neither a key nor
    another pattern taking over chasing a note from before it, Swing at 75 % delaying the step after the beat by half a step and Gate at 50 % halving a note
    (and a Gate of 50 % handed it as modulation halving one set at 100 %; through the synth, a macro's slot on Gate,
    the host's modulation of Swing and an LFO's slot on Velocity taking them where the telemetry says it plays them,
    and a macro's slot on the pattern picking B at its full knob and still A at 0.4, in whole steps),
    Reverse playing a note on the first step on the last (the playhead moving steadily backward, as on Ping-Pong's way
    back, and forward otherwise), a loop playing only the notes in it, a muted note and one at
    no chance silent and one at half playing about half the passes (alike each time), another pattern taking over where
    the loop comes round, in Key a key playing the pattern transposed from C4 (the scale's Root moving nothing) and stopping it when let go (its end
    reported, the sequencer's notes' never, the playhead in the telemetry, and the keys its notes are written on, not those they play), the keys the telemetry shows held leaving
    its notes out, and through the synth a note sounding from its sample however the blocks are split; its curves
    standing where the playhead is (back through a step played reversed, a bent segment off the straight line, held
    wrapping round the pattern's end from the last point to the first, a point past the end left out), doing nothing without points or while it doesn't play, and through the synth a bend curve
    bending what it plays by Bend Range and a mod wheel curve moving the mod wheel's slot, the telemetry telling the
    wheels what both add as they play and nothing once the key is let go or the sequencer is switched off; and a macro's
    curve taking the place of its knob wherever the knob is set, moving what the macro modulates and no other macro, the
    telemetry saying how far it moves the knob from where it is set and nothing once it stops; the Play button playing
    the pattern from its loop's start whatever Play says, as written but for Transpose, the keys left to play notes
    beside it (through the synth too, the telemetry saying it plays), nothing while the sequencer is off, and in Host
    from the loop's start, the transport taking it back once it stops; and from a step, its note at once and the one
    before it only as the loop comes round, reversed the way Reverse goes, swung its note at once too, and from a step
    outside the loop from the loop's start; Random picking among the steps notes start in, quarter notes playing one
    on every beat (several of them, alike each time), the playhead steady within a beat, and with notes at steps 0, 3
    and 8 each time round the loop starting on a note; and the loop shortened a step at a time under the
    playhead in its second time round (in Key, in Host and by the Play button) playing on from where it is, each note
    once, the transport jumping then locking it to the bars again; and a note's own bend
    planned from where the note starts, stretch after stretch, a note chased in its middle starting bent as far as it
    is there and one Gate shortens holding what it reached, and through the synth an A4 bending along its points, a
    sweep of two octaves in a step following its line within 0.3 semitones (not in stairs a stretch long), a chord's
    bent note bending its own voice alone, a mono glide starting where the note before was bent to, and a patch's
    bends loading back as saved;
  - the effects' latency, on an impulse through effects that run oversampled or wait and leave the sound as it is (a
    filter, an amp and a flanger at Mix 0): the input straight out beside a filter, the filter on and off, a split with
    an amp at 4x on one band and a filter on the other, two filters beside an amp at 8x, and nine filters in a row
    beside the input (a pad of nine paths), a shaper at 4x and at 8x beside the input, and a flanger through zero
    beside it at its shortest and longest Delay and switched off (as late each time), each flat within 0.05 dB from
    20 Hz to 18 kHz (the paths meeting in step) and as late as fxlag.h works out, and nothing oversampled on time;
  - the effects' newer controls: the reverb's Low Cut and High Cut taking its tail's ends away and Diffusion at 0
    leaving its first echoes spikier, its tail's echoes apart for longer (filling in as it rises, smooth and its sides
    unalike) and a click into it still ringing out; the delay's Duck taking its echoes away under a loud sine and
    letting them back after; Diffusion turned back to 100 % and Duck back to 0 landing on them exactly, at 44.1 to
    192 kHz and in pieces of 32 and 7 samples, so the reverb's mixing and the delay's ducking take their plain paths
    again; the shaper's Tone leaving what its curve leaves straight alone and changing what it
    drives, its oversampling changed under a sine ducking rather than clicking, and a hard-clipped C7 aliasing less at
    4x and 8x than at 2x; the compressor's detector high-pass keeping a loud bass from pulling a quiet treble down;
    the flanger through zero, held at its sweep's middle and negative, cancelling noise to nothing; the phaser's
    Spread moving its notches; and the Lo-Fi holding Bits' levels at Rate, Smooth taking the steps' edges off and Mix
    at 0 dry;
  - keyboard level scaling: an operator's level moving by its depth an octave from the breakpoint either side, evenly
    on the way, a linear side adding it every octave and an exponential one doubling all it moved every octave past the
    first (within 0.1 dB), held at its -96 dB floor; on a modulator taking its index down; off (no depth) leaving the
    sound bit for bit as it was wherever its breakpoint is, in host blocks and single frames; and its params' text
    reading back as written;
  - DX7 voices (`src/gui/dx7.h`): a bank dump, a single voice, both in one file and a bank without its header read
    back as written and packed as the DX7 packs them, junk giving none; and a voice played: INIT VOICE a sine at the
    note and an octave up transposed, its attack ramping in over the DX7's envelope step rather than clicking, a modulator at output levels 60 and 75 putting its first sidebands as far under
    the carrier as J1 against J0 at the DX7's index (within 1 dB), feedback 5's 2nd harmonic where feedback FM puts it,
    a decay at rate 50 falling 40 dB in the DX7's 0.62 s, velocity sensitivity 7 taking a note at 64 16 dB under one at
    127, keyboard level scaling at depth 99 (-LIN an octave out, -EXP two and three) within 1 dB of the DX7's and +LIN
    on an operator at full level lifting nothing, the pitch envelope at 74 playing 9 semitones up, the LFO at speed 35 at 5.6 Hz, and a bank's presets named
    for their voices in order and reading back as made;
  - the CPU cost of each factor.
- **UI snapshots** (`tools/uishot.cpp`): renders the editor offscreen to PNGs in `build\<config>\ui-shots`, in Midnight,
  the theme its pixel checks were written in, but for the checks and examples that pick another (the README's
  screenshots in Spectrum). On the way it replays drags and clicks:
  - the effects graph's slot grid: switching to it showing the chain as its main line, as wired; the main
    line's + opening the list of effects with 2-band split under them, the split coming with its join, its
    empty bands each curving out to a + on its row, each + ringed in its wire's color (read off the rendered
    pixels); a band's + putting an effect on its band; the join's node showing
    the split's card (at the inspector's right), its High knob setting the high band's level, in dB, and its S
    soloing a band, the split and join nodes marked with an S and the other band's node and wires faint, and the
    main line's wires grey, each band's and each half of the split's and join's nodes in its color, a split's coming
    in from grey at its left and a join's going out to grey at its right (read off the rendered pixels), and then
    the other band's too,
    both soloed; the split's crossover handle dragged on its card, and its slope picked from its list there, 18 dB refused; its title's
    2 | 3 switch splitting it in three, an empty middle band between shown (a click on its display left of, right of
    and between the crossovers showing the low, high and middle band), into the join's third input, its high
    crossover at 2500 Hz, its slope kept and its list refusing 6 dB and picking 24 (a split at 6 going to 12 as it
    splits in three), Low X turned up past the high crossover and High X down past the low one each pushing the
    other along, undoably (a run of wheel notches one undo step), an effect put on its + along the main line's row (and a second one after it, the other bands'
    wires into the join running level along their rows past it and their +s in their next places, read off the
    rendered pixels), its S soloing it and the low band's too, the row showing
    the container with it, and back to two asking where its effect goes (onto the low band's end), undone onto the
    middle band again, an empty one going back straight, all undone;
    + EFFECT with the split picked putting an effect at the end of a band, undone; the split taken
    out putting its bands' effects on the main line, undoably; a wire's middle lit under the mouse and opening the
    list, an effect going in between two effects, a split going in with its join right after it, and, from the wire
    into a join, an effect going at its band's end (the list offering no split there), each undone; a node under the
    mouse showing an x in a circle at its upper right (read off the rendered pixels), whose click removes it, undone,
    and an effect's a power symbol in a circle at its upper left (read off the rendered pixels), whose clicks switch
    it off and on, each undone,
    the inspector's cards having none, an effect's power switch in its corner instead; the split's node's x asking
    before it takes the split's effects (Cancel, Keep and Delete, each undone);
    nodes dragged and dropped while the others glide aside (polled until they settle), the wires running to the
    place a node would land from the glide's first step (read off the rendered pixels), its place staying open while
    it is held over a node: an effect along the main line, one onto a band's end by its wire into the join, one from
    the high band to the low band's end, and a split with its bands and join, carried by the join, to the line's
    start, each one undo step, undone, and one let go of with the mouse anywhere on a wire landing where it goes, no + showing while one is held, an
    empty band's and the main line's too (read off the rendered pixels); the mouse anywhere along a wire showing its + (read off the rendered pixels), a
    click there doing nothing and on the + opening the list, the wire's + and the line's faint until the mouse is on
    them, and a band's + after its last node showing always, an empty band's too (read off the rendered pixels); a node and its wire out glowing as loud as the telemetry says its
    output is, and not once it is silent or the glow setting is off (read off the rendered pixels); and going back to
    the row changing nothing, the split showing there as its container;
  - a macro's name that isn't valid UTF-8 leaving the rest of the editor drawn (read off the rendered pixels);
  - the output meter and scope moving repainting the header alone, a key, the Pitch Bend param and the host's bend
    on top of it (its wheel) the keyboard alone, the sequencer's bend and mod wheel curves moving their wheels the
    keyboard's panel alone, editing no param and each wheel back at its param as they stop (read off the rendered
    pixels), and an operator's scope
    the inspector alone, each painted over the frame before coming out as a whole paint does, pixel for pixel, and
    as notes play, likewise: an LFO modulating nothing moving repainting its card alone, on an operator's param its
    card and the panels' row, on the header's Pan its card and the header, on another LFO's Rate the two cards,
    operator A's envelope moving the inspector alone, and with the MODS view shown the panels' row too (the time a
    whole paint and those parts take printed); with no notes playing, a compressor's meter the panels' row and the
    host's tempo the whole editor;
    with none playing, a free LFO's dot moving repainting its card alone, and the editor telling the plugin while one
    runs (so it keeps processing), and its card repainted hundreds of times noting the cards no more often than a
    whole paint does; a retrigger one showing no dot, but while the effects' tail runs it its dot moving
    there, gone again with the tail;
  - the list of effects opened on a band's + closing at an undo, and an effect picked from it after its split went
    behind the editor's back going at the line's end;
  - a card dragged a tenth of a card onto its neighbor moving nothing, and three tenths making the neighbor glide aside
    from the first step (read off the rendered pixels), landing after it, undone;
  - a split in the row: the arrow between two cards offering it after the effects (its list kept inside the panel),
    putting a container there, its low band shown and empty; a card after it carried left over its band and controls
    landing between it and the card before it, and on over that card's left half before that card; the card before
    it carried over its controls staying, and on over its band and out of its right side landing right after it, and
    past the next card's middle after that card; each undone; with the split first in the row (put there by the arrow
    before the first card), a card carried left over its band and controls landing on the band, and on past the
    controls' left edge before the split, undone; a card dragged onto that band landing on it; the HIGH tab
    showing the high band (a click on the display left of the crossover the low band, and right of it the high band
    again), whose dashed card opens the list of effects (no split) and puts a reverb on it; the
    container's Low knob setting the low band's level, lowering that band's curve on its display (read off the
    rendered pixels), its S buttons soloing each band on its own, the band shown faint while the other is soloed (read
    off the rendered pixels), both undone, and its crossover handle dragged; the
    graph and back leaving every wire as it was; its fold showing while the mouse is over it or a card
    on its band (read off the rendered pixels); the container folded to a strip and unfolded; dragged by its title
    past the next card, with its bands, the card gliding into its place a step at a time (read off the rendered
    pixels), undone; no arrow into a band's dashed card (read off the rendered pixels); a card at its band's end
    moved right within its own place, or carried straight down out of the panel, moving nothing, carried over its neighbor and then the
    container's controls landing at its band's start (not before the split), and on over the other band's tab
    and let go of there landing at that band's end, the band shown left as it was, each undone; its x, kept, putting its bands' effects on the
    main line, the high band's first, undone; a card carried over the LOW tab lighting it (read off the rendered
    pixels), leaving the band shown, and let go of there landing at the low band's end; its title's menu
    offering Move to High only (Move to nothing on the main line's cards), which puts it at the high band's end,
    undone; and an arrow before the first card, and the arrows along a band in its color (read off the rendered
    pixels), the first's list putting an effect at the line's start;
  - a split's solo and band count buttons and a switched-off node's ghost showing their tooltips (read off the rendered
    pixels);
  - the effects graph's box taking the room the inspector's card leaves, more beside a narrow card than a wide one
    (read off the rendered pixels);
  - the FX panel's bin, in the chain and in the graph: asking first (read off the rendered pixels), a click around
    the dialog keeping the effects, going ahead removing every effect, a split and its join too, undoably, and with
    nothing left asking nothing (read off the rendered pixels); ten effects making the graph wider than its box, a scrollbar under it (read off the
    rendered pixels) whose thumb dragged to its end scrolls the graph there, a wide card's node and then a narrow
    one's picked there moving nothing, the + at the end then taking a click, and no scrollbar once the graph fits
    (read off the rendered pixels);
  - assigning LFOs and envelopes to a knob, a header knob and a wire label, and turning a knob;
  - connecting operators by dragging from a node's edge (lit under the mouse), anywhere around it, while a click
    there only picks the node and a drag from further inside moves it; a drag held back over its own node once clear of
    it looping round over its top and back into it, on the ring around it whether it is selected or not, and one only just past its edge showing no loop (read off the rendered pixels), and let go there feeding it back; a filter's edge dragged onto an earlier
    filter feeding it, and a filter's and a shaper's dragged back onto itself feeding it back; a filter going out by its edge dragged onto
    OUT, with nothing going in, and stopping by its arrowhead dragged away; routing a carrier through a filter,
    which goes out in its place, and picking waves and filter types;
  - moving routes by dragging their arrowheads onto another node and onto OUT, with their modulation, and
    removing one by letting its arrowhead go in empty space; a route's start and an OUT line's start dragged onto
    another node coming from there, with its amount and mode, undoably, grabbed 16 px along the wire and lit there;
    Ctrl+drag putting a node on the grid's nearest free point, the grid's points showing while Ctrl is held, and a
    plain drag leaving it off the grid; a drag called off as the mouse is taken away dropping
    nothing (an arrowhead held off the nodes, a source's handle over a knob) and ending a knob's gesture;
  - the MODS tab: a row per modulation, its Amount bar and aux's × changing it, and the switch after its source's
    name the source's polarity; its curve bent up and down by a drag, undoably, drawn in its source's color (read
    off the rendered pixels), and straightened by a double-click; + Aux's, the target's (three levels deep, into an operator's params) and the source's
    cascading menus picking an aux, a target and a source, the modulation keeping its depth, Amount and aux as it
    moves, undoably, and what modulates its depth following it (or, onto a macro or when it is removed, going); the wheel scrolling the rows; a row's typed depth, Edit depth and bypass dot (a macro's too); and a source's
    handle dropped on a depth handle becoming that modulation's aux, ringed in its color, the macro then no longer
    faint (read off the rendered pixels), and the depth handle hovered drawing the line from its source's handle
    (read off the rendered pixels);
  - the MODS tab's columns' names with no modulations (read off the rendered pixels);
  - an operator picked as a source from the MODS tab's source menu, given a target, bipolar to begin with, and its
    row's switch making it unipolar, undoably;
  - an operator's letter on its page lit under the mouse (read off the rendered pixels) and dragged onto a knob
    modulating it, undoably; and its handle and an LFO's held over a view's tab half a second showing that view (not
    before, and passed over the tabs showing none), let go of there modulating nothing, held as long over a node in
    the graph, an operator's and a filter's name in the matrix (a row's and a column's) and a node's tab atop the
    inspector showing that node in the inspector, still off (passed over one, not), and carried back still landing on a knob;
  - a MODS row's depth bar ticking where its modulation is now: an LFO's under a playing note (none with no note),
    a macro's from its knob (read off the rendered pixels);
  - a node switched on where another one shown sits coming back clear of it, undoably, and one whose place is free
    staying there;
  - a routing node under the mouse showing an x in a circle at its upper right (a filter's at its box's corner, read
    off the rendered pixels), whose click switches it off, undoably;
  - a wire's label sliding along its wire off a node sitting at its middle (the wheel there turning nothing), and
    two wires crossing at their middles, the second's label sliding along its wire off the first's, and a feedback
    loop's label going to the loop's left off a wire's label on its right;
  - the MODS view's filter: its magnifier opening it to take the keyboard, the words typed (every one, in any case)
    narrowing the rows, its × emptying it, Escape and a click elsewhere letting the keyboard go; and the target
    menu's search listing the targets found with their path, the first that can be picked lit (read off the rendered
    pixels), Up and Down picking one, Enter taking it, Escape emptying it and then closing the menu; an operator's
    targets in two columns, the envelope's beside the pitch's;
  - the modulation panel's + SOURCE opening the list of source types under it, adding Random and MIDI sources, each
    with its card, a MIDI card's knob picking
    its controller and its input's list pressure or slide instead, its CC knob going, and the list button's tooltip
    following its input; a Random card's Unipolar switch, under its clock list as a Curve card's, bipolar to begin
    with, setting its polarity and lit while unipolar (read off the rendered pixels), undoably, the wheel turning it on
    upward as it shows; a Curve source added second from the list,
    unipolar and widened, its Unipolar switch under its clock list, undoably, and folded showing
    CRV and its number (read off the rendered pixels);
  - with an LFO in each of the 32 places, a MODS row's source menu listing them in two columns of 16 inside the
    editor, the last place's at the second's foot taking the row's modulation;
  - adding an empty modulation in the MODS tab: it opens its source's menu, takes a source and modulates nothing,
    then a target from Global (which offers no Mod Wheel or Pitch Bend) makes it its source's modulation, each pick undoable, and
    its × removes it; one given a source that has rows going among them, its source menu, open over the rows,
    lighting none of them (read off the rendered pixels); a source card's button showing its modulations in MODS, or with none adding an empty one
    from it;
  - a wire label under the mouse opening into a pill (read off the rendered pixels): its mode part's list picking
    the route's mode and the wheel stepping it, undoably, its × removing the route (and an OUT label's its node's
    output), the open pill taking clicks and the wheel over the wire ends under it, which take them beside the label
    once it closes, and a resting label taking a drag over a wire's start under it;
    resting, a label showing a mode other than Phase (read off the rendered pixels); the mode also picked
    from its label's and its matrix cell's
    right-click menus (the cell's corner showing the glyph, read off the rendered pixels) and from the operator
    page's INPUTS tab, whose label sets the depth, each an undo step; a route moved onto another operator keeping
    its mode, and one disconnected starting as Phase again;
  - a node's line to OUT bent by dragging it sideways, from straight, undoably, and straightened again by a
    double-click;
  - bending a wire by dragging it sideways (lit under the mouse, its label following its middle), either way,
    undoably, back to its first bow by a double-click or Reset Bend, its bend going with it when it is
    disconnected, and one along the panel's top bowing only as far as keeps it and its label inside (read off the
    rendered pixels);
  - an operator lighting up while modulation opens its route from 0 %, and not with the glow setting off (read off
    the rendered pixels), and modulation dropped on a silent operator's OUT cell in the matrix showing its wire to OUT;
  - an operator's Pan knob turning from its middle and reset to it by a double-click;
  - the INPUTS tab's rows: a modulated depth's handles under it and its ranges just above it, its number left as it
    was (read off the rendered pixels), and eight rows scrolling under the wheel, the scrollbar's thumb and the
    middle button;
  - the node the inspector shows filled and ringed in the graph, picking another moving both, a dotted leader running
    from it across to the inspector's lens, and a switched-off one's ghost filled while it is shown (read off the
    rendered pixels); and the graph's glow dithered rather than stepped, as bright and as far out as the output is
    loud (what goes into the effects), rising to it over a few polls, as strong as the glow setting says, its level moving
    repainting the panel as a whole paint would (read off the rendered pixels);
  - a switched-off source's handle going faint, and an LFO's, an envelope's, a macro's and the note source's until
    they modulate something
    (read off the rendered pixels);
  - the matrix's cells keeping their width-to-height ratio, the mouse finding a cell across its width and not in the
    gap after it, and the cells and legend staying inside the panel (read off the rendered pixels);
  - the dot on a filter's and a shaper's tab while it sounds out, the output meter and its clip light, and the
    output scope beside it, empty with nothing captured and drawing a span at its peak (read off the rendered
    pixels), its height easing down to a quieter span, and its spans too small to draw painting nothing again;
  - with no note playing, Volume's knob showing the last voice's modulation while the effects ring out, and not
    after (read off the rendered pixels);
  - a shaper: switching it on with its tab, sending an operator and a filter into it (in the graph and the matrix)
    and running it onto an operator and into a filter, its page's curve list, link switch, symmetric switch (for
    a Custom curve) and knobs (Drive, Output and Mix, centered, with no To Out), its Custom curve drawn on the page and in the big editor, whose pencil sits beside the curve's list (on the
    shaper effect's card too), lit while it is open (read off the rendered pixels);
  - a shaper's Stack curve, between Fold and Custom in its list, showing its Stack stepper beside LINK, on its page
    left of it (the list narrowing for it, the knobs staying) and on the shaper effect's card right of it, the two
    centered together, its + an undo step a click, dragged in whole stages, undoably, and the wheel over the list's
    button stepping along the list's order;
  - the shaper effect's SYM switch, hidden but for a Custom curve, putting a point in the middle of its drawn curve, a double-click on the curve's
    mirrored left half doing nothing, the middle point refusing to be deleted, and the switch
    undoing with its point; the big curve editor's SYM (a shaper effect's and a voice shaper's) switching, its
    mirrored half taking neither a double-click nor a Shift+click's step, undone off again;
  - dragging the filter response handle, the filter's Mix knob (where Out was) moving its response and undone
    putting it back (read off the rendered pixels), switching between the graph and matrix views (the tab shown opening its
    panel's edge, and a hovered one's shape, read off the rendered pixels), and toggling a matrix cell: undone, its
    route (or one the wheel switched on) not kept at 0 %, and a press drifting a pixel setting nothing; a shaper's
    row routing it into the other shaper and, on the diagonal, into itself;
  - picking a filter's slope from its list, a band-pass's 6 and 18 dB refused, the hand cursor showing only over
    an entry it can pick (not over a refused one, the list's edge or around it); its type's list (as a shaper's
    curve's) showing the seven families with their icons (read off the rendered pixels) and picking one; the Model
    list beside the slope's picking a low-pass's Ladder, a family picked keeping the model where it has it (Ladder's
    high-pass and band-pass) and else taking its first; the wheel over the type's button stepping the family and over
    the Model button the model, a run of notches one undo step, held at the lists' ends, and the Model button's arrows
    stepping round its family; and the filter card's type list picking Comb, and its Model button, short and without
    arrows, picking a low-pass's Diode from its list; a morph's slope's list picking 24 dB (18 refused) and its notch switch beside it (the notch's icon)
    going through the notch and back, its six knobs, its Morph after Resonance turning, and a formant's, its Vowel after Q turning;
  - Points | Steps and Snap in the other big shape editors: Steps drawing a step in the note source's curve with a
    plain click, and Snap snapping a shaper effect's curve point in its editor;
  - points picked in a shape editor: in a Curve source's full-size editor a box from empty space picking the points
    it reaches (drawn filled, read off the rendered pixels), one of them dragged moving them all as far, one undo
    step, stopping as a whole at a neighbor not picked and at the top, Shift+click adding a point and taking one away,
    a click on one of several picked picking it alone, Ctrl+A and Delete removing all but the two kept, undoably, and a
    click on empty space letting them go; on a symmetric voice shaper's page the middle point picked with another
    staying in the middle; with the Curve card still widened beside the SEQ view, Ctrl+A over the roll picking its
    notes, not the shape's points, and Delete removing them; and on the SEQ view's BEND lane, with Select, a box picking its points (ringed, read off
    the rendered pixels), moved together and stopping at a neighbor, undoably, Shift+click adding one, with Draw a
    picked point moving the others too, Ctrl+A over the lane and Delete removing them, undoably, and a click off the
    lane letting them go, as Ctrl+A over the roll does (Delete then removing the notes);
  - making sources unipolar and bipolar from their menus (an envelope's and an LFO's card, a macro's knob and
    handle, the mod wheel and the note source's display);
  - dragging a Param EQ band's handle, and Alt-dragging it for its Q; its bands' chips: a click on one, its shape
    or a band's handle picking the band its knobs set, its power symbol and its number turning the band on and off,
    a switched-off band's handle pressed switching it on, undoably, and
    its shape's list (the third band's inside the card) picking the shape, a notch's handle moving only across, and
    its Amount knob setting the whole EQ's amount;
  - dragging an envelope's handles, pushed past the display's end too, where the envelope zooms out poll by poll
    to stay inside as the segment grows (read off the rendered pixels), its zoom held while the mouse stays over the
    display and easing back to filling it once the mouse leaves (read off the rendered pixels), and the attack's below 1 ms, to 0.5 ms and to 0 ms, drawn as a vertical line (read off
    the rendered pixels), and straightening a segment's bend to 0 % with a double-click, its readout drawn
    over the curve (read off the rendered pixels);
  - an operator page's ENVELOPE, UNISON and WARP tabs: a line across the envelope at the newest voice's level,
    and a dot on the curve along the attack, decay and release (one releasing from above the sustain too), as on a
    mod envelope's card (read off the rendered pixels), the unison knobs on theirs, the scope asking for its
    operator's output (and a noise operator's, or a hidden one's, for none), its trace drawn over 2 and 4 cycles
    and as a slow span fills, the faint plain wave with no note, a tab's ring, hidden and shown, while one of its
    knobs is modulated (read off the rendered pixels), and a dragged handle opening the tab it is held over after half a second (passed
    over one, not);
  - an envelope's curve following the modulation of its release and sustain, a faint line staying where the knobs
    set it (read off the rendered pixels);
  - an operator's Ratio readout: each digit dragged by its own place, carrying and held to 64.00, each drag an undo
    step; the wheel stepping the digit under the mouse, a double-click rounding to it, its depth handle dragging an octave every 60 px, the hovered digit lit (read off
    the rendered pixels), a typed fraction, and a source's handle dropped on it modulating it, its reach drawn along
    the track, a straight line to 64 with its tick at the end there and an arrow for modulation going on past it
    (read off the rendered pixels);
  - the WARP tab's warp: its buttons picking the warp (the Sync warp apart from the NOTE switch), its knob the amount
    and NOTE note sync (an eighth note, read off the rendered pixels), each an undo step,
    its display with no note drawing a square's pulse width as Squeeze takes it (read off the rendered pixels), a
    warp button's tooltip, the display synced, and a noise operator showing its envelope in the tab's place;
  - an operator's fixed pitch switched on by its keyboard (and by the wheel over it), Freq and Mult set to where the
    ratio was in one undo step and taking Ratio's and Fine's places, Mult stepping (and typed into, its × taken
    whole), and Pitch and Detune dimmed (read off the rendered pixels), not turning and not ringed for a dragged
    handle (read off the rendered pixels);
  - switching operators and filters on and off with their tabs, the matrix's names and their ghosts in the graph,
    and off from a node's right-click menu, a switched-off one's page dimmed (read off the rendered pixels);
  - the KEYS tab: its breakpoint dragged across onto whole notes and its depths up and down, each drag closing its
    gesture, a curve switch making its side exponential (read off the rendered pixels), the edits undone one at a time,
    a double-click putting the breakpoint back on C4, the newest voice's key as a dot on the curve and a hidden KEYS tab
    ringed while one of its knobs is modulated (read off the rendered pixels);
  - the UNISON tab starting on Alternate, its buttons picking the copies' curve (Supersaw) and pan (Ramp) and its
    Blend knob, each an undo step, the display's lowest copy moving from the right to the left (read off the rendered
    pixels), and a pan button's tooltip;
  - a stepper (the unison count's): its - and + stepping it, an undo step each, a quick
    second click stepping again and - doing nothing at 1 copy, its + faint at rest and lit under the mouse (read off
    the rendered pixels), the hand over its - and +, up and down over its number and while dragged, and the arrow
    over its label; and a number too long for - and + both (a fixed Mult's ×0.001) moving off the one under the mouse, which
    alone shows (read off the rendered pixels);
  - the play switches and the glide knob;
  - an LFO starting synced in quarter notes, and its sync switch freeing it; its Phase knob, marked on its display
    and faint while the LFO is free, and lit while it is anchored (read off the rendered pixels); its clock modes a
    list under its shape list, a pick undoable, the mode in force picked again leaving it, the wheel over it stepping
    along them and held at One-Shot, and its Unipolar switch beside its shape list, apart; a Random source's list, where an LFO's shape list is, Retrigger and Free
    alone;
  - the three-band dynamics' card: a band's row picking the band its knobs set, its threshold lines dragged
    across (Above stopping at Below) undoably, Alt-dragged for the ratio and double-clicked back, a line on another
    band picking that band, its zones dragged for their ratios (the upward one up for more, the downward one down, undoably, picking their
    band), its
    ratio drawn as up to seven lines moving with the threshold and closing up toward it (6 dB apart at 1:1), the Above knob's tooltip under its label
    and the bars' meters showing the telemetry's levels and gains, in teal, then amber for what is taken off (read
    off the rendered pixels), its band's Gain left out of them (read off the rendered pixels), and LINK (an edit of one
    band, by its line or knob, then setting every band's, undoably) and RMS;
  - the amp's card: added after the shaper in the list of effects, a Crunch into a closed 4x12 to begin with; its
    model list, cabinet list and mic switch picking, each undoably; its cabinet
    switched off dimming its controls (read off the rendered pixels), which still turn; its mic dragged on its display
    setting Position and Distance in one undo step and double-clicked back; its drive meter following the telemetry
    and its display following Treble and the mic's position (read off the rendered pixels); and its title's menu
    picking its oversampling, the one in force checked;
  - the arrows in a list's button: a filter's type (on its page and the filter effect's card), a shaper's curve (on
    its page and the shaper effect's card), the amp's model and cabinet, the reverb's algorithm and an LFO's shape
    stepping along the list, round its ends,
    each click an undo step (a quick second click, which comes as a double-click, a step too), the arrow lit under
    the mouse (read off the rendered pixels);
  - the utility's card: added last from the list of effects (under STEREO & PHASE), leaving the sound as it is (Mono
    Bass Off); the handles on its display setting Mono Bass, Pan, Width and Gain, each drag an undo step, the band
    narrowing into its middle below Mono Bass's line (read off the rendered pixels), and a double-click putting Mono
    Bass's back to Off;
  - the chorus's card: added from the list of effects after the flanger, with three voices; its 2 | 3 | 4 | 6 under
    its display picking its voices, each an undo step, its display drawing a lane a voice, six and then two, each at
    its place (read off the rendered pixels), and its Rate and Spread knobs turning;
  - the newer effects' cards (the widener's knobs, the frequency shifter's Shift, the phaser's SYNC, the
    flanger's STEREO (two speakers, read off the rendered pixels) and its + | − picking its polarity, the filter's Key Track and the shaper's and delay's LINK
    too) and the list of effects with their icons, each group's name on a strip, the splits' SPLIT group from the main line
    only (read off the rendered pixels);
  - a filter's and the filter effect's responses moving with the note their key tracking follows, and the filter
    effect's with its Mix (read off the rendered pixels);
  - the disperser's and widener's displays following their params as modulation takes them (read off the rendered
    pixels); the shaper's oversampling picked in its title's menu (2x to 8x, the one in force checked, undoably), its
    Tone, the delay's Duck, the compressor's SC HP, the reverb's Diffusion (its second row), Low Cut and High Cut (its third) and the
    phaser's Spread turning, the reverb's Mix at its card's bottom right, the flanger's TZF switching, and the Lo-Fi added from the list of effects, its knobs
    turning and its display following Bits (read off the rendered pixels); and the disperser's Key Track and Stretch
    knobs turning, each one undo step, Stretch in whole steps, its
    display's peak moving with the note Key Track follows and its copies drawn stretched (read off the rendered
    pixels);
  - switching effects with their cards' power switches and icons (a switched-off one's staying grey under the mouse,
    read off the rendered pixels), dragging cards by them, and folding cards, the icon staying put, one in a slot past the 16th
    too;
  - the right-click menu on knobs;
  - a macro's knob as a target: an LFO's and another macro's handle dropped on it, with their depth handles under
    it, its own macro's handle and one that would loop refused, its dot with no note playing showing where the other
    macro takes it (read off the rendered pixels), and the target menu's Macros listing the knobs by their names, the
    one that would loop and the row's own refused;
  - the macros' knobs (a double-click setting a bipolar one to its middle), handles and names (a name cut at the 24
    bytes the state saves between whole characters), the note source's
    and velocity's handles, displays and curve editors, the note source's and velocity's handles and the curves on
    their displays faint until they modulate something (read off the rendered pixels), the mod wheel's
    handle (new modulation starting upward) and the pitch bend's (bipolar), each over its wheel and faint until it
    modulates something (read off the rendered pixels); with no note playing, a knob's dot showing where a macro
    takes it (read off the rendered pixels); the octave shift's small buttons under the keys; a key clicked playing at
    velocity 1 at its top and 127 at its bottom (a black key along its own height), and a key slid onto as hard as
    where the mouse enters it; a key the host holds easing up to lit over a few polls (read off the rendered pixels); collapsing the keyboard
    strip to its top row by its chevron or title, the wheels' handles staying in it and still mapping the wheels; and
    Bend's knob, in the header after Pitch, turning; the bend wheel dragged editing the Pitch Bend param in one
    gesture (the host following it), springing back to the middle and leaving nothing to undo;
  - the dashed cards that add effects and sources (opening the list of types at the mouse, the effects' moved back
    inside the editor at the chain's right end and staying over a switched-off card's dimming, read off the rendered
    pixels; the sources' closing
    at a click around it, and shutting for good as the SEQ view covers their panel), and the arrow between two effects adding one there;
  - picking the oversampling, MPE and its bend range (leaving the patch alone), the theme (Spectrum, the default, and Porcelain first in its list), Cards' Plain | Tinted (disabled in Midnight) and the depth handles'
    size (drawn bigger at each, read off the rendered pixels), and dragging the hue slider (the accent taking its hue, read off the rendered pixels), the glow slider, the lines slider (a source's dashed line drawn as strongly as it says, and not at all when off, read off the rendered pixels) and the grid slider, a double-click putting each back where it starts and each showing its tooltip under it while the mouse rests on it and not while it is dragged (read off the rendered pixels), in the settings dialog (with the editor rendered in each theme, and glowing), its book opening the user manual and
    leaving it open, closing it with its x
    or a click around it, and the wheel scrolling nothing behind it;
  - undo and redo of drags, wheel runs, node moves, typed values and a removed effect with its modulation;
  - typing values into knobs, header knobs and target depths, with Enter, Escape and a click elsewhere;
  - the preset bar's arrows (asking first while the patch has changed, the dialog a line shorter before a file loads);
    its category's dot at its left, its name in the middle and its author at its right (read off the rendered pixels),
    and its tooltip; its info card's star making the preset a favorite, Edit Info, Show in Browser, Escape
    and a click around it closing it, and a factory preset's offering Save Copy; the browser opening on Factory, then where it was left, Show in Browser on the preset's
    folder, listing its subfolders' presets unless Subfolders is off, its names standing
    out from their authors in every theme (read off the rendered pixels), a click loading a preset and leaving it open, a
    double-click loading one and closing it, and Close closing it; its sidebar's All, Favorites and Recent, its tree's folders opening and closing and its categories
    filtering (two at once, Clear taking them off); the folder's path leading back up and the file's button
    showing the preset's file (none for a factory preset); its search finding presets by
    name, category, author and description; its + by FOLDERS making a folder and the Save dialog's + New a category;
    the x on one of the user's categories removing it after asking, its presets keeping it, listed in grey;
    Edit Info writing a preset's author and line into its file; the browser's tree's right-click menu opening a folder in
    Explorer, making a folder in it, renaming it (a name taken refused) and deleting it after asking (Escape keeping it),
    none on Factory; Tab and Shift+Tab stepping through the Save dialog's text fields, round their ends, each entered all selected (in Edit Info passing over the name); underscores typed into the Save dialog's name showing (read off
    the rendered pixels); a text field given UTF-8 (a preset's author from its file) cut, stepped through, erased and
    placed by the mouse between whole characters; Save As saving the patch as a new preset in its preset's
    folder with its info, as the patch's preset; the browser's Save Here beside Close, only on a folder of the user's
    and greyed out as Save As is (read off the rendered pixels), saving in that folder through the Save dialog; the
    Save dialog's folder, a list button:
    a factory preset's copy starting where the browser was left, its list scrolling under the wheel past eight rows,
    Factory's locked, a click picking a folder and closing it, Escape and a click around it closing it alone, and its
    New Folder making a folder in the one picked; Preset Info's folder moving the preset with its info, asking to
    replace a preset of its name there (the warning read off the rendered pixels), a failed move keeping it open; the
    author typed last starting a preset without one (Preset Info too), one with its own (a factory preset's copy too)
    keeping it and saving untyped remembering nothing; and every part (the bar, the info cards, the browser with its tooltips,
    filtered, the New Folder and New Category forms, the Save dialog with its list of folders and Edit Info) rendered in every theme
    (`fmvibe-uishot <dir> 1 --library` replays these alone); Save greyed out (read off the rendered pixels) and doing
    nothing while the preset is unchanged, asking before it overwrites the preset's file (Cancel keeping it, a failed
    write leaving the patch changed), on a changed factory preset opening Save As's dialog without asking, and doing
    nothing on Init, changed or not, where Save As does nothing until the patch changes and the preset browser, which
    loading a preset closes; its search, taking the keyboard once clicked and letting it go at a click elsewhere, narrowing the list as words are typed, Enter loading the preset found, Up and
    Down picking one, Escape and the search's × emptying it, and its editing: selecting with Shift and the arrows, Home
    and End, the mouse (a drag, a Shift+click, a double-click for a word), cutting and pasting;
  - drawing a Custom wave's partials, at 16 and 64 of them too (the wheel stepping through the counts), the ones a lower count hides kept as they were, the
    edit button (a pencil) by the keyboard switch opening the partial editor and closing it, as another node's tab
    in the inspector does (read off the rendered pixels), and the full-size shape editor hiding the inspector;
  - a Curve source's card: a click on its display widening it (the next card moving along), the inspector's knobs turning meanwhile, its shape's points added and dragged in the card, its
    tools inverting, reversing and shifting the shape (each one undo step) and copying it into another Curve source (one card
    widened at a time), its title row's expand button widening it as a click on its display does, Steps drawing a step with a plain click and Snap snapping a point unless Ctrl is held, its
    full-size editor greying the card's editor out (taking no clicks), its plot dimmed (read off the rendered pixels),
    until it closes, by its done button, a view tab or a click on that plot, the card staying widened, also while the
    SEQ view covers the modulation panel, and the
    card back to its size by its collapse button;
  - a Curve source's loop and Sustain: its card's clock modes a list where an LFO's shape list is, a pick, its
    arrows (round its ends) and the wheel (held at its ends) setting the mode, each one undo step, no tooltip under
    it (read off the rendered pixels), its Unipolar and sync switches under the list's right end, undoably, and its Length,
    Phase and Slew knobs alike beside it, Slew Off to begin with and dragged up undoably, an LFO card beside it
    picking its clock mode from a list of its own, its Unipolar switch beside it; its Rate, unsynced, shown and typed as the
    curve's length in One-Shot and Sustain (a length in s or ms, or a rate in Hz) and as a rate retriggered; its
    places reading as times unsynced (looped too), in bars and beats synced, else as a share at 0 Hz; its full-size editor's title row
    setting its clock mode from a list of its own (opening under its button, not the card's), its Unipolar and its sync,
    and its Phase and Slew knobs taking the wheel;
    its Phase a faint dashed line where notes start it, drawn solid while its knob is held, there and in the widened
    card (read off the rendered pixels);
    its loop's strip over the full-size plot, on the grid's columns: an end dragged
    sizing it, the start too, its middle moving it, a drag outside it drawing a new one, its ends a column apart and
    a double-click looping the whole curve, each one undo step, with the move cursor over it and the left-right one
    elsewhere; with Shift and with Ctrl, an end, the loop's middle and a new loop dragged off the columns, where the
    mouse is, each one undo step, the ends kept 1/256 apart; what each clock mode doesn't play dimmed and no bar one-shot (read off the rendered pixels); a point's
    Set as Loop Start and Set as Loop End putting that end exactly there, off the grid too, undoably, an entry that
    would cross the other end left out, and on a loop stored the other way round moving the end it says; the widened card's strip along its plot's top, none one-shot; and, let go in
    Sustain, its dot keeping to its curve while a dashed line across moves to the level it puts out (read off the
    rendered pixels);
  - an operator's ENVELOPE tab's ADSR | Curve switching to its drawn envelope in one undo step, drawn from its AHDSR
    the first time (its points, loop and Length; by the wheel too) and kept as drawn after; One-Shot | Sustain and its sync switch, its
    Length knob setting its length or, synced, its note value, and its Slew knob its slew, Off to begin with; its loop's end dragged on the tab's display onto the
    grid, one undo step; the newest voice's dot on it (read off the rendered pixels); and its pencil opening its
    full-size editor in the panel a Curve's takes (as wide as the routing panel, in the row's middle), laid out as a Curve's, its One-Shot | Sustain a list
    there, picked from undoably, the newest voice moving along it repainting the whole of it, its done button and a
    view tab closing it;
  - Copy Shape and Paste Shape: an operator's envelope copied from its menu (Paste Shape offered only once a shape is
    copied) pasting into a Curve source by Ctrl+V as it is, with its loop region, one undo step; into a symmetric voice
    shaper's curve its ends at the curve's and a point in its middle; a Curve source of 40 points into velocity's
    curve thinned to its 16; a symmetric curve without a middle point copied as it sounds there, and pasted over a
    Curve source of more points putting the points past it back at their defaults, undoably; into the BEND lane across
    the pattern, and the lane's curve copied back;
  - an LFO's shape list (going with the LFO undone, offering no Random or Custom shape, the wheel stepping along it
    to Square), a Random
    source starting synced and bipolar, its knobs, its dot waiting at the end of a cycle and, at 0 Hz, a line at the value it holds
    (read off the rendered pixels), the list of more waves (Organ among them), the shape editor's grid, snapping and reset button, a point dragged
    onto its neighbor's time, and
    scrolling the source cards by holding a dragged handle at the panel's edge;
  - scrolling the effects and the sources by holding a card being moved at either end of its row, where it lands; a
    source card moved within its own place, or carried out of its panel, moving nothing;
  - two fingers panning the effects' row (the knob the first finger pressed left alone), the source cards, the
    preset browser's list and the effects graph, and nothing over a dialog; the wheel turned sideways scrolling the
    row, the source cards and the graph; one finger turning a knob it lands on (on a card in the row too, a double
    tap setting it back), tapping an effect's icon, and elsewhere panning the row, the source cards and the
    browser's list (picking nothing it went down on, a tap then picking one);
  - Ctrl-dragging an effect card along the row, an effect's node and a split's (its join and its band's effect along)
    in the graph and a source card, each leaving a copy where it lands with the original's settings, one undo step;
    a plain drag moving instead, and a Ctrl-drag called off leaving nothing, not even an undo step;
  - Enter and Escape on the bin's dialog, the settings and the preset arrows' and the browser's "load anyway?"
    (over the browser, its search letting the keyboard go): Escape cancelling or closing, Enter going ahead;
  - dragging the effects, the source cards, the MODS view's rows and the preset browser's list and sidebar with
    the middle button held: over a knob without turning it, stopping at the row's end and following back from there at once,
    and doing nothing while the left button drags a knob;
  - eight modulations on a knob and six on a matrix cell showing as many depth handles as fit and a "+n" pill,
    nothing drawn outside the knob's column or in the next cell (read off the rendered pixels); the pill's list
    showing six rows (read off the rendered pixels), scrolling under the wheel and the middle button, bypassing,
    setting the depth of and removing its modulations and closing at a click elsewhere, opening under a
    macro's handle held over the pill and taking it as a row's aux, widening for an aux column whose chip (read off
    the rendered pixels) and its × take the aux away, and a source's line ending at the pill holding its handle;
  - a depth handle's right-click menu removing its aux, undoably, offered only while it has one;
  - a hovered depth handle drawing its range wider, over the others' (read off the rendered pixels);
  - a knob's modulation ring cut off at the knob's end ending in a line across it, and one within range not (read
    off the rendered pixels);
  - in Spectrum, a source card outlined only while the mouse is over it (read off the rendered pixels);
  - with tinted cards on, Spectrum's and Porcelain's cards tinted with their own color and Midnight's and Light's left
    as they are (read off the rendered pixels);
  - a card title's fold and x showing an action's face only under the mouse, in Midnight and Spectrum (read off the
    rendered pixels);
  - a card's fold drawn only while the mouse is over the card and its x always, in Midnight and Spectrum, the hidden
    fold still folding it, and showing with no mouse once a finger has touched the editor (read off the rendered
    pixels);
  - in every theme, Porcelain among them, a filter's and a shaper's display on the inspector's lens standing apart from
    the lens's face (read off the rendered pixels);
  - in Spectrum, a modulated knob's arc keeping the accent and a ring from a source of a like color (LFO 1's) drawn
    lighter, and Midnight's ring in its source's own color (read off the rendered pixels);
  - in the flat themes, every source in a color of its own and no effect in the dim text's, and an aux that is set
    a chip tinted with its source's color, standing out from its MODS row (read off the rendered pixels);
  - an aux's handle hovered drawing a dotted line to the depth handle it scales, and that depth handle hovered
    drawing it from the aux's handle (read off the rendered pixels);
  - an LFO's handle showing its "Drag to assign" tooltip only until it modulates something (read off the rendered
    pixels);
  - every factory preset's routing graph rendered (in Spectrum, into `presets/`), none with a node on or near
    another, a wire, a line to OUT or a label over a node it doesn't join or through a loop, two labels on each
    other, or two wires running on each other or into a node from nearly the same way, their arrowheads together
    (`src/gui/arrange.h` counts them; `fmvibe-uishot <dir> 1 --presets` renders and checks these alone;
    `--seq` renders the SEQ view of each one that plays the sequencer, its roll fitted to its notes as a preset
    load fits it);
  - every algorithm (`src/gui/algorithms.h`: six operators' 32, four's 8, three's 5 and two's 2, set by
    `RoutingModel::applyAlgorithm()`) on a new patch: laid out with
    no clash as arrange.h counts them, found again as the one the operators form, and taken back, params and
    layout, by one undo; and onto a patch of its own, a route it has kept at its depth, the others disconnected with
    their mode and modulation (undone back), a carrier going out through a filter left so and counted as one, the
    filters and the carriers' sends left alone, and a four-operator one switching E and F off and a modulator's send
    into a filter that goes out disconnected;
  - ALGORITHM, at the tabs' row's right with ROUTING and MATRIX (not FX or MODS, another view closing its panel):
    its panel drawn over the graph, the foot naming the thumbnail under the mouse, a click setting the operators to
    it in one undo step, closing the panel and putting its number on the button, the one in force lit when it opens
    again (read off the rendered pixels), the 4 of its Operators switch showing the four operators' eight (the panel
    opening on them while one is in force), its 3 and 2 theirs, which switch the operators past theirs off, Escape and a click
    around closing it without a change and Enter left to the host;
  - the title bar's tempo: for a plugin host, its H lit to begin with and the field showing the host's tempo dimmed
    (read off the rendered pixels), taking no drag, wheel or typing; the H's tooltip and a click on it turning it off,
    unlit, the field undimmed (read off the rendered pixels); for a host that lets the editor set its tempo, no H (read
    off the rendered pixels) and the field the synth's own from the start; the synth's own tempo dragged a BPM every 2 px (a tenth with Shift), the wheel stepping it a BPM a notch onto
    whole ones (a tenth with Shift), held to 20-999, and typed after a double-click or Ctrl+Click (`BPM` after it
    taken, text that isn't a tempo and Escape leaving it), none of it touching the patch, its undo history or the
    host's tempo;
  - the SEQ view covering the modulation panel, as tall as the editor was, its source cards and title taking no clicks;
  - the SEQ card's Play button: with the sequencer off doing nothing, and the whole view dimmed but the power switch
    and the size button (read off the rendered pixels) and taking no input (a double-click on the roll, a drag, a
    pattern button, Ctrl+A, the roll's menu), the size button and the power switch still working; on, a click playing the pattern and a second stopping it (also before the telemetry has caught up), lit
    while the telemetry says it plays (read off the rendered pixels), editing no param and leaving nothing to undo,
    starting at the first note picked within the loop (none, the loop's start; two, the earlier); and
    a note's end dragged past the pattern's end, the note dragged right and moved by the Right arrow stopping at it;
  - the SEQ card's size button and a double-click on its tab (while the view shows) switching the view to its half size,
    the modulation panel back under it (read off the rendered pixels) and the overview gone, and back, kept in the user's
    settings with no param edited and nothing to undo, and a double-click whose first click shows the view leaving it;
    Speed, Length, Octave and Transpose as steppers, Length's + and - stepping it, and Transpose's track filled to its
    value with no line across it (read off the rendered pixels); and a macro's handle dropped on the card's Gate knob modulating it, its ring drawn (read off the rendered pixels),
    undoably, and on Octave's nothing, and on PATTERN's buttons modulating the pattern picked, undoably, its reach and
    depth handle showing under the row, above PLAY (read off the rendered pixels), and the button it picks ringed, with
    nothing playing where the macro takes it and as the telemetry tells while it plays (read off the rendered pixels);
  - the notes' own bends: the Bend tool after Select and Draw putting a point down on a note by rows (from a point at
    its start), heard at the pitch it bends to (`Controller::auditionBend()`), its line drawn over the rows (read off
    the rendered pixels), with Alt off the grid, a point dragged stopping at its neighbor, a segment's middle bent and
    straightened by a double-click and a point double-clicked going, Shift+drag drawing the bend a point a grid line;
    the field's Bend scaling and clearing the picked notes' bends (dragged down past nothing taking them away and back
    up in the same drag bringing them back); a bend copied, pasted, stretched by x2 and removed
    with its note; folded to a scale, a row up a degree of it; and the mouse over a bend's line showing a faint point
    there and following it along the line, the editor told to paint again as it moves (read off the rendered pixels),
    on the snap's nearest line, as a press there puts it (with Alt at the mouse),
    and the arrow, a press there putting a point down on the line, a note without a bend showing its line flat along
    its pitch (read off the rendered pixels), which takes the mouse and a press as a bend's line does, the notes faint with
    Bend and the bend of the note under the mouse lit (read off the rendered pixels), a bar its bend has left taking no
    press and showing the arrow; and bend points picked by a box (ringed, read off the rendered pixels), moved
    together, Shift+click letting one go, Ctrl+A and Delete removing them, each an undo step, and a point dragged past
    the roll's top scrolling the rows, the point going along;
  - the SEQ view's PATTERNS: a pattern's notes drawn in its tile where an empty one's has none (read off the rendered
    pixels), what the roll shows a rectangle in the shown pattern's tile, narrowing as the roll zooms in, and no other
    tile changing (read off the rendered pixels), a click on a tile picking its pattern, undoably, a tile carried onto another ringing it (read off the
    rendered pixels) and let go of there copying its pattern, undoably, let go of on its own place copying nothing, and
    the pattern that plays showing the playhead in its tile (read off the rendered pixels); a tile's menu offering Copy
    Pattern, Paste Pattern once one is copied and Clear Pattern while its pattern holds notes, pasting over an empty
    pattern without asking, over one holding notes asking first (Escape keeping it, Enter overwriting it, undoably), a
    tile dragged onto one holding notes asking too, Clear Pattern asking and clearing, undoably, Ctrl+C and Ctrl+V over
    tiles copying and pasting, and the rectangle dragged in its tile scrolling the roll two steps;
  - the SEQ tab, between FX and MODS (a note put down by a double-click and held dragged across lengthening it, to the right its end, to the left its start, the cursor left-right until the mouse moves and while it goes across, and up-down while it goes up and down): its card and an empty roll, and a dot on the tab while the sequencer is on (read
    off the rendered pixels), as on FX's while an effect in the line is switched on; a slight line parting the roll from the lane in every theme (read off the rendered pixels); a double-click putting a note down on its step (a
    step long at 1/16) and on the note deleting it; the notes and the playhead where the telemetry puts it (read off
    the rendered pixels); a note the playhead reaches glowing, and not with the glow setting off (read off the rendered
    pixels); a note landing on another's start and pitch (dragged, or by the arrows) taking its place,
    undoably; a note dragged past the roll's top scrolling the rows, the note going along; a box held there scrolling them too, its start staying on the roll, and picking a note that was above the rows with one that showed from the start; one held past its right edge, zoomed in, scrolling it on by a step or two in half a second, not to the pattern's end; a patch loaded fitting the
    roll to its notes; a note dragged moving on the grid by steps and rows, undoably, its end dragged setting
    its length and its start moving it with its end staying (the cursor left-right over either end), and either with Shift set off the
    grid, as a note moved with Shift lands (a Shift+click on a note picked, not dragged, unpicking it); a box picking the notes it reaches and the field's VEL dragged up turning every one picked up; the
    field's VEL and OFFSET double-clicked back to 100 and onto the grid; a key the sequencer plays lit in the roll's
    keys, easing up over a few polls and back down once its note ends (read off the rendered pixels); the loop dragged by its ends and its middle (with their cursors) and a new
    one drawn across the bars outside it; the draw tool's cursor a pen over the empty grid (and over a note what a drag does to it), a note it draws and holds
    taking the velocity the mouse sets up and down (one undo step with the note), and with Shift painting a note in
    each step dragged across; the card's switches showing tooltips of their own (read off the rendered pixels); the
    roll zoomed in across by its +, Ctrl+A picking every note and the arrows moving them a step, a semitone and with
    Shift an octave (the rows following only as far as shows every note moved; with none picked, going on to the
    host), each undoably, a note moved up or down heard at its new pitch for 300 ms (the editor's clock moved
    on), Left and Right hearing nothing; a picked note's dot
    ringed in the lane (read off the rendered pixels) and, with Draw, the picked notes' velocities dragged together
    there (where notes start together, the picked one's alone), the
    lane's cursor a pen and up and down over a dot and while one is dragged; with Select, a box from the lane's empty
    space picking the notes whose dots it holds (ringed, read off the rendered pixels; over their stems alone, none), a dot dragged moving
    every one picked together, one undo step, Shift+click adding a dot's note and Ctrl+click letting it go, a click on
    one of several picked picking it alone, and the arrow over empty space; Delete removing the notes picked; a note or key clicked in the
    roll heard alone while held (not played as the keyboard's, a key clicked quickly twice heard twice), a note moved heard on its new row; the middle button
    panning it across and up and down, and Alt+wheel zooming the rows
    in about the row under the mouse, the rows' bar at its right scrolling them; the scale's list opening under its button in the shortest editor (the keyboard collapsed); a note put down by a double-click heard
    while the button is held; the lane
    on CHANCE setting a note's chance; on BEND the lane's grid lines (read off the rendered pixels), with Select a
    click doing nothing (no pen) and a Ctrl+double-click putting a point down on the grid and a Ctrl+drag moving it
    onto the grid (the cursor moving over it), undoably, with Draw a click putting one down where the mouse is (a pen)
    and a double-click there leaving it, a segment's middle dragged up bending it (the cursor up and down) and
    double-clicked straightening it, the last segment's too, round the pattern's end, with Select a point double-clicked going, and the curve drawn where the lane was
    bare (read off the rendered pixels); on MOD WHEEL Draw's Shift+drag drawing a point a step; the MACRO tab looking as the lane's other tabs do, lit only while picked and with no border (read off the rendered pixels), its label and arrow lit apart under the mouse (read off the rendered pixels), a click on the label showing its macro with the list shut and one on the arrow opening it, the arrow pointing down while it is shut and up while it is open (read off the rendered pixels), its list of macros picking the one the lane shows, a point drawn on its curve alone, undoably, a macro with a curve showing it small in its row and its dot full where one without has it faint, on the tab and in the list (read off the rendered pixels), a click around the list and Escape closing it, the wheel stepping the tab along the macros, and where the curve holds the macro's knob showing on the knob with no param edited (read off the rendered pixels); a double-click on the ruler looping the whole pattern; the scale's list picking Minor and FOLD showing
    only its rows, and a note off it keeping its row, hatched (read off the rendered pixels); the snap's list, and the roll's lines falling at its length and as strong as the GRID setting
    says (read off the rendered pixels); no copy button by PATTERN, A's tile dragged onto an empty B copying A there; and the bin clearing the pattern,
    asking first, undoably.

  Pass a scale to render larger: `build\release\fmvibe-uishot.exe out 2`.
- **VST3 smoke test** (`tests/vst3_host.cpp`): a small headless VST3 host. It loads `FMVibe.vst3` and checks what the
  VST3 side adds to the engine the tests above check:
  - the module's exports and its factory: one class, an Instrument|Synth at the project's version, which a host can't
    ask for an interface it doesn't have; the component being its own controller, with MIDI mapping, note
    expressions, units and its process context needs (the tempo, and the transport's state and position);
  - the buses (a stereo output, refusing mono, and a 16-channel note input) and 32-bit processing only;
  - the params: every one hosts are given, fewer than the engine's, with the CLAP plugin's ids, all different, each in a unit the plugin lists, going to
    normalized and back unchanged (a stepped one exactly), their defaults, a value's text and back, Voices stepped
    and not automatable, the MIDI controllers' params hidden, the effects, the sources' own fields and the slots'
    wiring not given to hosts (their depths and Amounts given), and every param they are given showing; the MIDI
    mapping (per channel, on the note input only), the note expressions and the
    physical UI mapping;
  - playing: an A4 at 440 Hz, bent through the MIDI-mapped pitch bend, the MIDI-mapped mod wheel (CC 1) moving the
    Mod Wheel param and the host told in its place in the block, tuned an octave by a note expression on its id
    (and not by one on another id), its level set by a volume expression, automation landing at its place in a
    block, a param change coming before a note at the same place, notes let go (with ids and without) falling
    silent, an anchored LFO standing where the process context's position puts it, more changes than a block sorts
    (a note's end past them still ending it, a param's last point winning), a flush without audio taken, the
    controller's copy of a value from the host left alone while processing, and a voice limit past the voices
    allocated, stepped up a block at a time, asking the host for one restart once it has settled (the plugin's
    window's timer), the latency the CLAP plugin reports (the voices' decimator's), asked for
    while inactive what the next activation makes it, a note held as a state with a shaper effect loads holding the
    restart it asks for off until its key is let go, then playing on through it (setProcessing(false),
    setActive(false) and back) in its release with no new attack, processing stopped and started again with that
    restart asked for and none made ending the note, and
    `setActive()` out of memory answering `kOutOfMemory` and going through once there is room;
  - the state: the CLAP plugin's bytes, through a stream that moves 5 bytes at a time, the controller's own state
    empty, a value from the host taken while not processing, loading bringing the saved value back and having the
    host read the values again, and a foreign blob refused;
  - the editor in a hidden host window: one at a time, resized keeping its aspect ratio, a preset loaded by the
    preset bar's arrow with no edits (kParamValuesChanged and the project marked changed instead), the Volume knob dragged
    (beginEdit, performEdit, endEdit, and an older value the host hands back right after not taken), a state with
    the keyboard strip collapsed asking the host for a shorter window (as wide, or held within a small screen's work area), its window gone once it is removed, and
    opening again after.
- **User manual** (`tools/manual.py`, `cmake/manual.cmake`): turns `docs/manual.md` into the one self-contained
  HTML file the release ships, `FMVibe-Manual.html` in the build folder (its styles inline, the screenshots as data
  URIs, system fonts, so it opens offline), and fails on a link to a heading that isn't there (the manual's links
  use GitHub's heading ids), an image that isn't there, a link to a file, which the page can't carry, or Markdown
  it couldn't convert. It reads only what the manual uses: headings, paragraphs, lists nested by two spaces, tables,
  fenced blocks, images, links, bold, italics and code; something else the manual takes up needs a line there. It
  needs Python 3 and nothing else, and without Python the test is left out (CMake says so).

More checks:

- **Validators**: `.\build.ps1 -Validate` runs clap-validator (free-audio's conformance tests, a prebuilt binary)
  on the CLAP plugin and Steinberg's validator (the VST3 SDK's, built here from the SDK's MIT-licensed sources with
  the SDK's own list of them) on the VST3 one. They are the `validate-clap` and `validate-vst3` targets, both under
  `validate`. Steinberg's validator lists notes ("Info") beyond its tests: param titles alike within a unit, values
  that come back slightly off from their rounded text and units with many params are expected.
- **In Bitwig Studio**: install the plugins as the [README](README.md#build) describes; Bitwig finds them
  in the per-user CLAP and VST3 folders.
- **Without a DAW**: `build\release\fmvibe-uihost.exe` (`tools/uihost.cpp`) is a small CLAP host for trying the
  editor by hand. It loads the `FMVibe.clap` next to it (or the path it is given), opens the editor in a window of
  its own, which keeps the editor's aspect ratio as it is resized and takes the sizes the editor asks for, and plays
  it through the default output device (WASAPI, shared mode, at the device's rate). Notes come from the editor's
  keyboard, from the computer keyboard as in the browser demo (A W S E D … from middle C by key position, Z and X
  shifting the octave and C and V stepping the velocity through 1, 20, 40 … 100, 120 and 127, both of which the
  window's title shows; keys the editor takes for typing stay its own) and from every
  MIDI input it can open, at a tempo of 140 BPM for synced LFOs and effects, its transport stopped at its start (so
  a sequencer set to Host plays only by its Play button, and anchored LFOs run free). It offers the plugin FMVibe's own host extension for the tempo (`src/hosttempo.h`), so the
  editor's title bar shows the tempo, to change as it plays (the plugin playing its own, with no H). With
  `--silent`, or without a device, it still runs the plugin in real time into silence, so the meters, the scope and
  the analyzers move; `--no-midi` leaves the MIDI inputs alone and `--tempo <bpm>` sets another tempo. It restarts
  the plugin when a higher voice limit asks for it, and runs its main-thread callbacks and timers. It isn't a test, and like a
  DAW it is for a person to start: it waits for its window to be closed. `.\build.ps1 -Run` builds it and starts it
  after the other steps.
- **What a change costs**: `.\tools\bench.ps1 -Commits HEAD~1,HEAD` (or `HEAD,Working` for the working tree's
  changes, new files too, or `-Plugins` for builds made already) builds the CLAP plugin at each commit in a worktree
  of its own under `build\bench`, keeps each build there, and runs `fmvibe-bench` (`tools/bench.cpp`) on them. It
  renders every factory preset through each build, as saved and at 8 unison copies, and says whether each plays bit
  for bit as the first does. Then it times the builds in turns, round after round, on one logical processor (`-Cpu`,
  2 to begin with): 16 notes on the default patch, the smoke test's chain of six operators at 1 and at 8 unison
  copies, nothing playing, the chain decaying and releasing, and the 1-copy chain again with 8 host events a block
  (values that change nothing, which only splits the block, then automation and modulation that move their params),
  each in ms of CPU per second of audio, the median of
  the rounds, with its change against the first build. It warns when the machine was busy. `fmvibe-bench` talks to
  the plugin through CLAP alone, with the param ids host projects store, so it runs on earlier commits' builds too.
  Not a test: timing wants a quiet machine.

## Linux development build

Linux builds both plugins: `./build.sh` makes `FMVibe.clap` and the `FMVibe.vst3` bundle (the object in
`Contents/x86_64-linux`, with `Contents/Resources/moduleinfo.json`), the engine and the editor (`fmvibe-core`, drawing
through Cairo in `src/gui/canvas_cairo.cpp`), the audio quality test, the UI snapshots, the preset tool and a small
test host (`cmake/linux.cmake`, which `CMakeLists.txt` takes in on any system but Windows). The editor is an X11 child
window of the host's (`src/gui/editor_x11.cpp`), on XWayland too: no Wayland and no GTK is linked. The file and
warning dialogs are the desktop's (`zenity`, else `kdialog`), run as separate programs beside the host's UI thread, so
only X11, Cairo and fontconfig are loaded into the DAW; without either program the editor says so for a moment and
nothing opens. It needs CMake, Ninja, a C++20 compiler (GCC 13 or Clang 18), pkg-config, git, Cairo (`libcairo2-dev`)
and X11 (`libx11-dev`), and fetches the CLAP SDK and the VST3 interfaces with git clones of the tags `CMakeLists.txt`
pins, since the cloud sessions' proxy refuses GitHub's archive downloads. The manual's check takes Python 3 too, where
there is one.

```bash
./build.sh                  # build/linux: build, then ctest (quality, ui, manual, linux-host, vst3-validator)
./build.sh --skip-tests     # build only
./build.sh --debug          # build/linux-debug
./build.sh --shots 2        # render the snapshots only, at 2x, into build/linux/ui-shots
./build.sh --install        # and put FMVibe.clap in ~/.clap, FMVibe.vst3 in ~/.vst3
```

- **What leaves a module**: only the entry points (`clap_entry`; for the VST3 bundle `GetPluginFactory`, which the SDK's
  module loader also reaches through `ModuleEntry`/`ModuleExit`), through the version scripts in
  `cmake/symbols-clap.map` and `cmake/symbols-vst3.map`; the engine's own symbols stay local, so a CLAP and a VST3
  loaded in one host process can't bind each other's. The plugins link libstdc++ and libgcc statically
  (`-static-libstdc++ -static-libgcc`, `FMVIBE_STATIC_RUNTIME` in `cmake/linux.cmake`), as the Windows ones do the CRT,
  so a distribution whose GCC is older than the one that built them still loads them.
- **The validator**: Steinberg's VST3 validator is built from the SDK's MIT-licensed `vst3_base` and `vst3_public_sdk`
  sources (`cmake/vst3-validator.cmake`) and runs on the bundle as the `vst3-validator` test, and with `--target
  validate`; it is what showed that a Linux VST3 module must export `ModuleEntry`/`ModuleExit` as well as
  `GetPluginFactory`. `-DFMVIBE_LINUX_VST3_VALIDATOR=OFF` leaves it out.
- **The editor's window**: an X11 child of the host's window, painted into a Cairo image buffer and copied in whole or
  by what changed, so a frame is never shown half drawn; the host's timer (CLAP's timer-support, VST3's run loop)
  pumps its events, poll and paint. `tests/linux_host.cpp` opens it in a window of its own, checks that it paints, and
  with `FMVIBE_SHOT=file.ppm` writes what it shows; then opens it again under Xlib's own error handler (which ends the
  process), at a scale given before its window (opening at that size), and destroys the host's window before the
  editor, the host still running after. It reads a window's pixels only once the window shows and only what of it is
  on the screen (`grabShown()`), as a window manager (a desktop's, WSLg's) shows the host's window a moment late.
  It also checks that, with no view open, a state's restart comes through a run loop the host application offers
  as `initialize()`'s context, on its main thread, and through the plugin's own thread where there is none; and that
  under a locale writing a decimal comma (de_DE, compiled with `localedef` into a folder of its own where none is
  installed, left out where it can't be), the user's settings file's numbers read as written (a saved zoom of 0.6
  and of 0.75 opening the editor as wide as they say).

- **Looking at the editor**: the snapshots are PNGs in `build/linux/ui-shots`, one or more for each step that
  `tools/uishot.cpp` replays; an agent can open them as images. To see a change, render the snapshots and open the
  ones its replay saves, or add a replay that saves one.
- **What differs from Windows**: the text is drawn in Inter (`FMVIBE_FONT` picks another family), not Segoe UI, so it
  comes out a little wider, and Cairo's antialiasing isn't Direct2D's. A UI check that reads a pixel should read it
  clear of text and of edges whose place follows a text's width. The checks that read where a text's ink falls use
  `checkFont()`, which only checks on Windows: a Linux machine has a zoo of fonts, whose metrics move the ink by a
  pixel. A lit ring's feathering is the backend's too: past the ring the Cairo halo is the background.
- **Not covered**: the Windows smoke tests, the CLAP validator (clap-validator comes as a Windows binary), `fmvibe-uihost`
  and `fmvibe-bench` (with `tools\bench.ps1`) need Windows, as does `src/gui/editor.cpp` (the Win32 window) and
  `src/gui/canvas.cpp` (Direct2D).
  `build.ps1` on Windows (or the release workflow) is the full check there; the Linux build compiles the shared files
  and its own window.

## Factory presets

The factory presets are preset files, `presets/Factory/<name>.fmv`, which the build compiles in:
`cmake/embed_presets.cmake` turns them into byte arrays (`factoryPresets()` in `src/factory.h`), for the plugin and
the browser demo alike, again whenever one changes. Neither writes them out: the preset library lists them from
memory under Factory, beside the preset folder's files (`PresetLibrary::rescan()`), and a factory preset's "file" is
`factory:` and its name (`PresetLibrary::kFactoryPrefix`), which the host's state names as it would a file. So a
name, once released, is stored in projects: none is renamed or taken out. The one exception: up to 0.11 each name
began with its category's two letters ("LD Screamer"), and since then the names go without them; such a name, from
a project or the old `Factory` subfolder, finds the preset as it is named now (`factoryPresetNamed()`). `presets/factory.txt` names them in the
order they came; a new one goes at the end. The build fails on a file the list doesn't name, or a name without its
file.

They are stored sparse: only the params that differ from their defaults (which is what loading a preset gives the
others), then the editor layout, a kilobyte or so where a preset the plugin saves holds every param (about 128 KB).
To add or change one:

1. Make it in the plugin and save it into `presets/Factory` (the preset bar's Save, then the file dialog).
2. Make the file sparse: `build\release\fmvibe-presettool.exe "presets\Factory\<name>.fmv"`. A factory preset says
   it is FMVibe's, in a built-in category, with a line about it (the quality test checks): set them with
   `--author=FMVibe --category=<Bass, Drum, FX, Keys, Lead, Pad, Pluck or Seq> --description=<one line, at most 80
   characters>` before the file. With `--arrange` first
   (`fmvibe-presettool --arrange <file>...`), it also lays the routing graph out again where its nodes, wires, lines to
   OUT or labels run over each other (`arrangeRouting()` in `src/gui/arrange.h`: each node moved to the place on a
   grid and each wire given the bow that lowers a cost most, from where they are and from columns by their distance to
   OUT), and says how many such clashes are left. The UI checks fail on a factory preset that has any.
3. For a new one, add its name at the end of `presets/factory.txt`.
4. Build and run `ctest`.

A factory preset changed here reaches users with the next release. Being built in, it can't be saved over: Save
on one opens Save As (`PresetStore::presetWritable()`). Versions up to 0.11 wrote the factory presets out into the
preset folder's `Factory` subfolder: the scan leaves the files there named as a factory preset out (and the subfolder,
unless it holds presets of the user's own, which it lists), and `PresetLibrary::resolve()` takes a file there named as
a factory preset for the built-in one, so a project naming it loads the built-in one.

Their Volume levels them to about -14 LUFS (integrated, K-weighted and gated as BS.1770 has it): three single
notes, an octave apart and a fifth, held 2 s (0.75 s for a Pluck, Drum or Keys preset), a Bass an octave lower, a
riser by where it ends up. None peaks past -1 dBFS at full velocity: not a single note anywhere from C2 to C6, held
and let go, nor, but for a Drum preset (played a hit at a time), a major triad at C3, C4 or C5 struck together, as a
host's quantized chord is (with the transport playing). One that would stays that far under, so the presets with
sharp attacks and the polyphonic Keys, Pads and Plucks, whose chords add up, come out quieter: Comb String, Pluck
Delay, Koto Pluck, Mallet Steps and Razor Pluck 7 to 13 dB under it. A chord of four notes can still go past
-1 dBFS. The level was set by ear on Clubbing, Randomizer, Kick 1 and Snare 1, which sit within a couple of dB of
it. A new preset comes in at about that level, and is tried by ear; the quality test fails on one whose triads peak
past 0 dBFS.

Factory presets are not pinned by checksum, so changing or adding one needs no test edit. Don't add a checksum test
for them: a preset is meant to change as its sound is tuned, and a pinned hash only forces an edit to the test on
every change.

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
- So a browser must never mix files from two builds, which GitHub Pages' caching (10 minutes a file, each on its
  own timer, and no headers of our own) would otherwise allow. The build writes `version.txt`, a hash of every file
  in the site (`web/version.cmake`); `index.html` fetches it past the cache and loads `style.css` and `main.js` with
  `?v=` and the hash, and `main.js` takes that query off its own URL for every file it loads: the modules,
  `canvas2d.js`, `worklet.js` and both `.wasm` files (the editor's through Emscripten's `locateFile`). A new file
  the page loads takes the version too.
- Presets are the factory ones, held in memory. Load, Save and the preset folder show a message instead, since the
  demo has no files. The theme, hue, glow, handle size and line strength are kept in the browser's storage.
- The computer keyboard plays notes: A W S E D … from middle C, with Z and X to shift the octave and C and V to step
  the velocity (1, 20, 40 … 100, 120, 127; 100 to begin with). The editor's keyboard plays as hard as where its keys are
  clicked.
- On a touchscreen, `main.js` hands the editor one finger (`View::touchDown()` and on: it drags what drags, pans
  what scrolls, and taps), with its own slop and double tap, preventing its moves so the page doesn't scroll. Two
  fingers are the page's: the canvas's `touch-action: manipulation` lets the browser scroll and pinch-zoom the page
  with them, so a phone can always scroll past the editor. A second finger landing (on the canvas or off it), or a
  move the browser won't let be cancelled once the finger is past the slop (the page scrolling already), ends the
  first's gesture with `View::touchCancel()`, and the editor takes nothing more until every finger is lifted; a
  finger left over from two, while the page isn't scrolling, is cancelled too.
  `View::touchPanBegin()` and on, the plugin window's two-finger pan, go unused there (uishot still tests them).
- The page's Full screen button puts the whole page (so the editor's menu and messages show) in full screen through
  the Fullscreen API, with the `webkit` calls as a fallback, and hides where neither is (an iPhone's Safari). In full
  screen, `resize()` fits the editor to the screen, keeping the top right corner clear for the button, and asks for
  landscape (`screen.orientation.lock()`, where the browser takes it).
- The page stands in for a host's transport: its `Controller` offers the tempo (`hasTempo()`), which is the synth's
  own (`ownTempo()`; `tempoSync()` is never on there, and the title bar shows no H), for the editor's title bar to
  drag, scroll or type (20–999 BPM, kept in the browser's storage), and which `dsp_tempo()` hands the synth as
  `Plugin::process()` does the host's tempo. Its transport is stopped, so a sequencer set to Host plays nothing by
  itself and anchored LFOs run free. Instead `dsp_restart()`, which `dsp_init()` and the worklet call as the page loads
  a patch (an example, or Init), stops the sequencer's Play button and, for a patch whose sequencer is on and set to
  Host, presses it after the patch's first block (once the sequencer has read that it is on), so such a preset plays
  its pattern from its loop's start as it loads, the button lit.
- The page polls and paints at most about 60 times a second, whatever the display's rate, and, like the plugin
  window, repaints on a mouse move only when what the editor shows changes.
- The page ships FMVibe's `LICENSE` (as `LICENSE.txt`) and `web/site/THIRD-PARTY-NOTICES.txt`, the licenses of the
  code its modules hold from CLAP, Emscripten and musl, both linked from its footer. When `build-web.ps1` pins
  another Emscripten version, the notices take that version's `LICENSE` and `system/lib/libc/musl/COPYRIGHT`.
- WebAssembly can't flush denormals to zero, which `Plugin::process()` does. Instead, the effects' input, the
  voice filters' input and the decimators get a signal far below hearing, behind `__EMSCRIPTEN__`. Without it,
  tails ringing out cost many times the CPU. The plugin's build is unchanged.

## Releasing

A version tag publishes a release. `.github/workflows/release.yml` builds the plugins on Windows (Visual Studio
2026) with `build.ps1 -Validate`, so every test and both validators have to pass. It packs each format into a zip
of its own: `FMVibe.clap`, `LICENSE` and the CLAP SDK's license (`LICENSE-CLAP`) into
`FMVibe-<tag>-windows-x64-clap.zip`, and the `FMVibe.vst3` folder, `LICENSE` and the VST3 SDK's license
(`LICENSE-VST3`) into `FMVibe-<tag>-windows-x64-vst3.zip`, each with the user manual, `FMVibe-Manual.html`
(`tools/manual.py`, from `docs/manual.md`). Beside it, on Ubuntu 24.04 (whose GCC 13 is the oldest the Linux build
takes, so the plugins need a glibc about as new as its, which the job's log prints), it builds them with `./build.sh`
under Xvfb (so the test host opens the editor), every Linux test and Steinberg's validator passing, checks that they
don't link libstdc++ or libgcc_s, and packs them the same way into `FMVibe-<tag>-linux-x64-clap.tar.gz` and
`FMVibe-<tag>-linux-x64-vst3.tar.gz`, each holding a folder of its name. It then creates a GitHub release for
the tag with the zips, the tarballs and notes generated from the commits since the previous tag. Meanwhile it builds
the browser demo on Linux, with the Emscripten version `build-web.ps1` pins, and publishes it on GitHub Pages
(`https://pcdlin.github.io/FMVibe/`), with the manual beside it as `manual.html`, which the README links to: the
manual online is the released version's, as the download is. Nothing goes out unless the plugins on both systems and
the demo build.

`.\release.ps1` does all of it: it needs `main`, a clean working tree, `main` level with origin and a free tag. It
raises the minor version in `project()` at the top of `CMakeLists.txt` (`-Version 1.0` picks another), runs
`build.ps1 -Validate` (every test and both validators), and copies the snapshots the tests render, in the Spectrum theme, over the README's:
Screamer's routing view over `docs/editor.png`, Crystal Motion's effects' panel as a chain over `docs/fx-chain.png`, and
Neon Pluck's effects' panel, a graph split low and high, over `docs/fx-graph.png`, and Future Plucks' SEQ view over `docs/seq.png`. It then lists what it would do and asks; only an answer of `yes` commits
`CMakeLists.txt` and the four screenshots as "Version X.Y" on `main`, tags it `vX.Y` and pushes `main` and the tag.
Any other answer puts those files back as they were. `.\release.ps1 -Screenshots` does only the screenshots, on any
branch: it builds and runs `fmvibe-uishot` and copies its renders over the four, leaving the version, the other tests
and git alone.

By hand, instead:

1. Set the new version in `project()` at the top of `CMakeLists.txt` (and where `docs/features.md` names it), and
   commit.
2. Tag that commit with the same version and a `v` before it, and push the tag:

   ```powershell
   git tag v0.2
   git push origin v0.2
   ```

The workflow refuses a tag that doesn't match the version in `CMakeLists.txt`. If a UI check fails on GitHub's
runner, the run keeps the editor's snapshots as its `ui-shots` artifact (`ui-shots-linux` from the Linux build).

To try the workflow before tagging, run it by hand on a branch (*Actions > Release > Run workflow*, or
`gh workflow run release.yml --ref <branch>`): it builds, tests and packs on both systems and the demo, keeping the
zips and tarballs as the run's `plugin-windows` and `plugin-linux` artifacts (named after the branch), and skips the
release and the Pages deploy.

Before the first release, the repository needs two settings:

- *Settings > Pages > Build and deployment*: Source **GitHub Actions**.
- *Settings > Environments > github-pages > Deployment branches and tags*: add a tag rule for `v*`. GitHub creates
  this environment allowing only the default branch, and a deploy from a tag is refused until it is added.

On GitHub's free plan, Pages needs the repository to be public.

## Debugging in a host

Install the debug build (`.\build.ps1 -Config debug -Install`), which installs both plugins. Bitwig runs plugins in
a separate process, so in Visual Studio use *Debug > Attach to Process* and pick `BitwigPluginHost-X64-AVX2.exe`;
breakpoints in `src/` then hit.

## Layout

| Path | What |
| --- | --- |
| `src/plugincore.cpp` | What both plugins are: `PluginCore`, the engine and its param values, the editor's `Controller` (presets, settings), the state and the editor window, telling each host through its hooks |
| `src/entry.cpp` | The CLAP plugin's exported `clap_entry` and factory |
| `src/hosttempo.h` | FMVibe's own CLAP host extension for the tempo, which `fmvibe-uihost` offers so the editor can show and set it |
| `src/plugin.cpp` | The CLAP plugin: the core with its process loop, events and extension tables (ports, params, state, remote controls, voice info, gui) |
| `src/vst3entry.cpp` | The VST3 plugin's exported `GetPluginFactory` (and `InitDll`, `ExitDll`) and factory |
| `src/vst3plugin.cpp` | The VST3 plugin: the core as one object that is component, processor and edit controller (buses, process loop, params in units, MIDI mapping, note expressions, state), and its editor view |
| `src/vst3iids.cpp` | The ids of the VST3 interfaces the plugin and its test host use |
| `src/state.cpp` | The state's bytes: the patch (params, then the editor layout chunk, whose fields one list describes for writing and reading), the settings and preset chunks; independent of Windows |
| `src/remotepages.cpp` | The remote control pages hosts show, built from the patch and the editor's source order |
| `src/presets.cpp` | The preset library: the factory presets, built in, and the preset folder and its scan |
| `src/usersettings.cpp` | The user's settings file in the roaming app data (theme, hue, glow, handle size, line strength, the editor's size, whether the keyboard strip is collapsed, favorite and recent presets, the user's own categories, where the preset browser opens, the author typed into the Save dialog last and the SEQ view's size), which processes share under a lock, each merging its own edits into the file as it is |
| `src/sharedtelemetry.h` | The telemetry the audio thread hands the editor, in atomics |
| `src/params.h` | Parameter layout (index ↔ id), slot targets, value mappings |
| `src/params.cpp` | Parameter table (names, ranges, defaults) and value text |
| `src/uistrings.inc` | The wording of every piece of text the plugin shows, by name; `src/uistrings.h` looks it up |
| `src/synth.cpp` | FM voices, matrix, filter routing, LFOs and mod envelopes, telemetry; independent of the plugin formats |
| `src/sequencer.h`, `src/sequencer.cpp` | The sequencer: plans the notes of its pattern for each stretch the synth renders, from the host's transport or a key |
| `src/wavetable.cpp` | Band-limited operator wavetables, one per octave |
| `src/zerobuffer.h`, `src/zerobuffer.cpp` | Delay lines (floats, or doubles for the phase disperser's stretched all-passes) allocated as zero pages that only take memory once written |
| `src/filter.h` | State-variable and comb filters, plus their response curves for the editor |
| `src/clipcurve.h` | The shapers' curves, anti-aliasing and running, shared by the shaper effect and the voices' shapers |
| `src/effects.h`, `src/effects.cpp` | The effects on the voices' sum: `Effects`, its slots (each holding every effect type's state, so it switches type without allocating, and running its type through a static dispatch in `runSlot()`) and the effects graph that runs them (wires, splits, sums, the paths kept in step) |
| `src/fx/` | Each effect type a slot runs, in a module of its own (`shaper`, `delay`, `eq`, `parameq`, `filterfx`, `compressor`, `multidyn`, `reverb`, `phaser`, `flanger`, `chorus`, `shifter`, `disperser`, `widener`, `utility`, `ampfx`, `lofi`): its state, its `prepare()`, `start()` and `run()`, and the responses the editor draws of it; `common.h` what they share (the `fx::Context` a slot hands its effect, the `fx::Meters` it writes, biquads and crossovers, ramps, delay line reads) |
| `src/amp.h`, `src/amp.cpp` | The Amp effect's sound: its models (preamp stages, tone stack, power amp), cabinets and mics, its oversampling, and the responses the editor draws (`src/fx/ampfx.h` runs it in a slot) |
| `src/analyzer.cpp` | The spectrum analyzer (FFT) behind the Param EQ's, filter effect's, phase disperser's and voice filters' displays |
| `src/envelope.h` | AHDSR used by operators and mod envelopes |
| `src/drawnenvelope.h` | An operator's drawn envelope: its shape stepped a sample at a time, its loop in Sustain, and its release on to the shape's end |
| `src/fxlag.h` | How late each path through the effects graph is from the effects that run oversampled, the pads that keep paths meeting in step, and the output's latency |
| `src/unison.h` | Where an operator's unison copies go: their detune, place and level by its Unison Curve, Pan and Blend, for the voices and the UNISON tab alike |
| `src/warp.h` | An operator's warps: where along its cycle it reads its wave, how much they squeeze it and where they bend it |
| `src/gui/view.cpp` | The editor: its widgets, drawing and mouse and keyboard handling, reading the patch through the models below, and handing its panels (`src/gui/panel.h`) what is theirs. Window-free, so it also renders offscreen |
| `src/gui/widget.h` | What the editor is made of for input, shared by the `View` and its panels: the `Widget` and its kinds, the mouse's modifiers and cursors, the keys, a context menu's entries and the text fields |
| `src/gui/panel.h` | The editor's areas as classes of their own: the `Panel` interface each implements (its widgets and popups, painting, cursor, polls, mouse, wheel, keys, text fields, pans, context menu, a source's handle dropped on it and the questions it asks), the `PanelHost` through which it reaches what the `View` keeps for every area, the drag going on as a panel is handed it (`DragState`), what a panel keeps for a paint or hit test (`ScopeCache`), and how a row of cards scrolls under a drag held at its edge (`edgePull()`) |
| `src/gui/patchedits.h`, `src/gui/patchedits.cpp` | What the editor's models of the patch stand on: the params read and edited through the `Controller` as the editor edits them, the layout saved with them, and the messages an edit refused leaves. Window-free and without a `View`, so a tool can edit a patch through them |
| `src/gui/modulationmodel.h`, `src/gui/modulationmodel.cpp` | `ModulationModel`: the sources in their places, their slots' targets, depths, auxes, bypass and polarity, what a source may modulate, the MODS view's empty modulations; assigning, moving, retargeting and removing modulations, adding and removing sources |
| `src/gui/routingmodel.h`, `src/gui/routingmodel.cpp` | `RoutingModel`: the routing graph's nodes on and where they sit, its routes and outputs (shown, kept at 0 %, how far their wires bow), connecting, rerouting and disconnecting them, and setting the classic algorithms |
| `src/gui/fxgraphmodel.h`, `src/gui/fxgraphmodel.cpp` | `FxGraphModel`: the effects' line of effects, splits and joins, read from the wires and written back to them; filling, adding, copying and removing effects, and adding, splitting in three and removing splits |
| `src/gui/theme.h`, `src/gui/theme.cpp` | The editor's color themes, the colors of the one being painted, and the graphs' glow |
| `src/gui/layout.h` | Where the editor's panels, cards, buttons, lists and dialogs go, in logical pixels |
| `src/gui/icons.h`, `src/gui/icons.cpp` | The editor's icons (effects, filter types, band shapes, LFO modes, waves) and the point cache behind them and the graphs' curves |
| `src/gui/routing.h` | The routing graph's geometry: which param joins two nodes, wires and arrowheads, their bows and grips, a node's edge, and where the inspector's lens's leader starts |
| `src/gui/routingpanel.h`, `src/gui/routingpanel.cpp` | The routing panel, `RoutingPanel`: the routing graph and the matrix, with ALGORITHM and its panel of algorithms: the graph's nodes, wires and labels (clear of each other), its glow (the effects graph's backdrop too), halos and grid's points for Ctrl+drag, the drags that move nodes and routes and bend wires, a label's list of modes and the routing's context menus |
| `src/gui/inspectorpanel.h`, `src/gui/inspectorpanel.cpp` | The inspector, `InspectorPanel`: the lens over the routing panel's left end and the page of the node it shows (an operator's waves and their list, its tabs, unison copies, scope and INPUTS rows, a filter's response and handle, a shaper's curve), its title and nodes' tabs; what covers it, and a node's tab or name clicked showing that node |
| `src/gui/filterdisplay.h`, `src/gui/filterdisplay.cpp` | A filter's display, shared by the inspector's filter page and the filter effect's card: its response with its modulation's reach, the spectrum behind it and its handle |
| `src/gui/lists.h` | The lists the editor's list buttons open: which params open one, their entries' values and names, and which have arrows that step along them |
| `src/gui/fxpanel.h`, `src/gui/fxpanel.cpp` | The FX view, `FxPanel`: the effects' line of effects, splits and joins as it shows it: its two views, the row (each split a container holding its bands) and the graph's slot grid with its +s and inspector, their cards' and nodes' moves (and Ctrl-drag copies), glides and scrolling; a split's controls, which both show; the list of effects, the bin and its questions. What an effect's card holds below its title row is still the `View`'s |
| `src/gui/fxgraph.h` | The effects graph's look (its bands' colors and the splits'), shared by the editor's files |
| `src/gui/shapeeditor.h`, `src/gui/shapeeditor.cpp` | The drawn shapes' editors, `ShapeEditor`: Curve sources' shapes, shaper curves and the note source's and velocity's curves, edited in place (a widened source card, an effect's card, a voice shaper's page) or in the full-size editor over the panels' row, and a Custom wave's partial editor; which are open, the points picked, Points \| Steps and Snap, the grid, symmetric curves, and the shared painters of a shaper's curve and a widened card's shape |
| `src/gui/shapeclip.h` | The drawn shape Copy Shape keeps, shared by the shape editors and the SEQ view's lanes, and how a paste fits it to where it goes |
| `src/gui/modspanel.h`, `src/gui/modspanel.cpp` | The MODS view, `ModsPanel`: every modulation as a row, its filter, and the cascading menus (the target's with a search) that move it, retarget it and pick its aux |
| `src/gui/seqpanel.h`, `src/gui/seqpanel.cpp` | The SEQ view, `SeqPanel`: the sequencer's settings card, and the piano roll with its tools, ruler, keys and lane |
| `src/gui/dialogs.cpp` | The settings and confirm dialogs, the preset bar's actions (Init, Load, Save, Save As, the arrows), and the text fields' editing |
| `src/gui/dx7.h`, `src/gui/dx7.cpp` | DX7 import: sysex banks and single voices read (and written, for the tests), and a voice turned into a patch through the editor's models, its envelopes traced as the DX7 runs them; the plugin's Load and `fmvibe-presettool --dx7` import through it |
| `src/gui/presetbrowser.h`, `src/gui/presetbrowser.cpp` | The presets panel, `PresetBrowser`: the preset bar's info and its card, the preset browser (its sidebar, list and search, its folders' menus) and the Save dialog (which also edits and moves a preset) with its list of folders, and the New Category, New Folder and Rename forms |
| `src/gui/canvas.cpp` | Small Direct2D/DirectWrite drawing layer |
| `src/gui/canvas_web.cpp` | The same drawing layer over the browser's 2D context, for the demo |
| `src/gui/canvas_cairo.cpp` | The same drawing layer over Cairo, for the Linux development build |
| `src/gui/editor.cpp` | Win32 child window that hosts the view in the plugin window |
| `src/gui/editor_x11.cpp` | The same on Linux: an X11 child window over Cairo, the zenity/kdialog dialogs, the popup menus, the clipboard |
| `src/gui/controller.h` | What the editor needs from the plugin: the `Controller` (values, edits, telemetry, layout, settings), the `PresetStore` it reaches the preset library through (`NoPresetFiles` for a host without files), the user's `EditorSettings` and the `UndoHistory` each `Controller` keeps |
| `src/gui/controller.cpp` | The undo history, which each `Controller` keeps for the editor |
| `src/factory.cpp` | The factory presets' loading; the presets themselves are `presets/Factory/*.fmv`, built in by `cmake/embed_presets.cmake` (see [Factory presets](#factory-presets)) |
| `presets/` | The factory presets' files, and `factory.txt`, the order they came in |
| `tools/presettool.cpp` | `fmvibe-presettool`: makes preset files sparse, as the factory ones are stored, lays their routing graphs out (`--arrange`) and sets their info (`--author`, `--category`, `--description`); with `--dx7=<folder>`, imports DX7 sysex files into presets there |
| `src/gui/arrange.h` | Laying a routing graph out: its clashes (nodes, wires, lines to OUT, loops and labels over each other, wires on each other or meeting a node together) and a search for a layout without them |
| `tests/smoke_host.cpp` | Headless CLAP test host |
| `tests/vst3_host.cpp` | Headless VST3 test host |
| `tools/uishot.cpp` | Offscreen editor renderer and interaction checks |
| `tests/linux_host.cpp` | Headless host for the Linux CLAP and VST3 plugins (loads them, plays, checks the editor paints) |
| `tools/uihost.cpp` | `fmvibe-uihost`: a small CLAP host with a window and audio, for trying the editor by hand |
| `tools/bench.cpp` | `fmvibe-bench`: what a build of the CLAP plugin costs (`time`) and what it plays (`render`), to compare builds by |
| `tools/manual.py`, `cmake/manual.cmake` | The user manual (`docs/manual.md`) as one self-contained HTML file, for the zips and GitHub Pages, and the `manual` test that checks it |
| `tools/bench.ps1` | Builds commits (or the working tree) and compares them with `fmvibe-bench`: bit for bit, then CPU, round-robin |
| `build.ps1` | One-shot configure/build/test/validate/install |
| `build.sh`, `cmake/linux.cmake` | The Linux development build: the engine, the editor over Cairo, the quality test and the UI snapshots (see [Linux development build](#linux-development-build)) |
| `web/dsp.cpp`, `web/ui.cpp` | The browser demo's audio and editor modules |
| `web/site/` | The demo page: the canvas and input (`main.js`), drawing (`canvas2d.js`), the AudioWorklet (`worklet.js`) |
| `release.ps1` | Cuts a release: version bump, tests and validators, the README screenshots, commit, tag and push (see [Releasing](#releasing)) |
| `build-web.ps1` | Builds the browser demo with Emscripten, and serves it |
| `.github/workflows/release.yml` | On a version tag: a GitHub release with the plugins, and the browser demo on GitHub Pages |

Everything except the plugins' own files builds into the `fmvibe-core` static library, which the plugins and the
tools share. `plugincore.cpp`, `presets.cpp`, `usersettings.cpp` and `editor.cpp` build into `fmvibe-plugin` on top
of it, which each plugin links a copy of (so the editor's window class belongs to that plugin's DLL). The VST3 SDK's
interfaces and their ids build into `vst3-interfaces`, for the VST3 plugin and its test host. The browser demo builds the engine (`FMVIBE_ENGINE_SOURCES`) into its modules
itself.

## Plugin facts

- CLAP id `io.github.pcdlin.fmvibe`, VST3 class id `9BCBBCB9-166D49F6-A9C2271D-5009BDD5` (`kClassId` in
  `src/vst3plugin.h`). Changing either makes hosts treat it as a different plugin.
- The VST3 plugin is a single component: one object is its component, processor and edit controller, so
  `getControllerClassId()` has none to name, and the controller's own state is empty (the component's is the whole
  state). Its params are FMVibe's, by the same ids, normalized over each one's range (a stepped one's steps spread
  as VST3 has them; the retired chain order has more orders than a step count holds, and passes as continuous). After them
  come the MIDI controllers' params (ids from `0x40000000`, `kMidiParamBase`: 130 per channel, CC 0-127, then
  channel pressure and pitch bend), which `IMidiMapping` hands hosts and the process loop turns back into MIDI;
  they are not stored. Param changes and events are sorted by their place in a block and applied there, param
  changes first; past the first 4096 (`kMaxChanges`), the rest apply after them, in the host's order. While
  processing, the host's values reach the params through `process()` only: its copy for the controller
  (`setParamNormalized()`) may come late, and is taken only while not processing.
- The version number is set once, in `project()` at the top of `CMakeLists.txt`. It reaches the code as
  `FMVIBE_VERSION`: the plugin descriptor reports it to hosts and the settings dialog shows it. FMVibe is released
  under the MIT license (`LICENSE`).
- Parameter ids are stored in host projects and automation. `src/params.h` derives them per section with spare
  room: globals 0+, operators 100 + 50·op (their filter and shaper sends, then Fixed, Freq, Mult, Phase, Ratio, Pan,
  Sync, Warp, Warp Amount, Unison Curve, Unison Pan and Unison Blend, then the drawn envelope's EnvMode, EnvSustain,
  EnvLoopStart, EnvLoopEnd, EnvLength, EnvSync, EnvDivision and EnvSlew, ids 137 to 144 + 50·op, then keyboard level
  scaling's KeyBreak, KeyLeftDepth, KeyRightDepth, KeyLeftCurve and KeyRightCurve, ids 145 to 149 + 50·op, which fill
  the operators' room; the
  9th field, once Velocity, does nothing and is hidden, kept so the ids after it stay, as are the first two, once
  Coarse and Fine, since Ratio after Phase), matrix
  400 + 10·from + to, filter modulation
  500 + 10·filter + to, shapers 600 + 50·shaper, filters 800 + 20·filter, filter links 900+, effect slots
  1400 + 80·slot (their fields past
  the 80th 12000 + 20·slot, past the 100th 25000 + 40·slot, past the 140th 36000 + 20·slot, and past the 160th
  36400 + 40·slot; the slots past the
  first 16 every field
  26000 + 200·(slot - 16)), source places
  2800 + 40·place, freeform shapes 4000 + 100·place (their points past the 33rd 15000 + 100·place), the sources'
  later slots 6000 + 30·place (the places past the first 16 every field 58000 + 100·(place - 16), and their shapes
  every field 60000 + 200·(place - 16)), shaper effects' curves 7000 + 50·slot (their points past the 16th
  17000 + 200·slot; the slots past the first 16 every field 30000 + 200·(slot - 16)), the chain's order 33400 + place,
  slot bypasses 9000 + 20·place (then the macros', the note source's, the mod wheel's,
  velocity's, the pitch bend's and the operators', 9320 + 20 a source by the sources' number, then the places past the
  first 16, 9680 + 20·(place - 16)), Custom waves' first 32 partials
  10000 + 100·op, macros 11000 + 40·macro, the note source 11400+ (its slots, then its range and curve), the mod
  wheel's slots 11500+, velocity 11600+ (its slots, then its curve and Bipolar), the pitch bend 11700+ (its slots,
  then Bipolar), the operators as sources 57000 + 40·op (their slots, then Bipolar), the
  Custom waves' partial counts 14000+ and their partials past the 32nd 14100 + 100·op, the routes' modes
  700 + 10·from + to (from by the routing graph's node order: the operators, the filters, the shapers; to an
  operator; an operator's own field, its feedback, hidden and always Phase), the voice shapers'
  curves 13000 + 50·shaper (their points past the 16th 21000 + 200·shaper), every slot's aux and Amount
  22000 + 40·source (a pair per slot, by the sources' stored number, see below: the first 16 places, the macros, the
  note source, the mod wheel, velocity, the pitch bend, the operators, then the places past the 16th; the blocks fill
  their room, so a source added later needs a range of its own), and every slot's curve 24000 + 20·source (by the
  sources' stored number likewise), and every slot's aux depth 49000 + 20·source (likewise); the effects graph's splits 34000 + 20·split, the wires into its
  slots and splits 34200 + 20·node (the slots, then the splits; a From and a Level an input) and the wires into its
  output 35000+, its sums 35100 + 20·sum and the wires into them 35200 + 20·sum. A wire's From stores its port + 1
  (0 = none): the input, then each slot's output, each split's bands, low to high, then each sum's output. The graph's
  nodes are numbered the slots, the splits, the output, then the sums. The editor wires a line (see
  `src/gui/fxgraphmodel.cpp`): a split's two bands are its low and high ones (Bands 2), and its join is the sum of its
  number, whose first two inputs take the high band and the low band. The effects always run as the graph: a chain
  is a line of it (the output's first wire comes from the input to begin with), and the FX view's row and graph are
  two views of it, the view a layout field (`UiLayout::fxGraphView`). The chain's places (33400 + place) are only
  read when loading a state from before version 9. The sequencer's settings are 40000+, each pattern's length and
  loop 40100 + 10·pattern, each pattern's notes 41000 + 10·(128·pattern + note), and each pattern's curves
  47000 + 200·(2·pattern + curve), the bend's then the mod wheel's, and each macro's 50000 + 200·(8·pattern + macro), a
  point count and 64 (x, y, curve) points each (see `SeqParam`, `SeqCurveParam`), and each note's bend
  64000 + 32·(128·pattern + note), a point count and 8 (x, y, curve) points (see `SeqNoteBendParam`). The
  operators' drawn envelopes' shapes are 81000 + 200·op (`opEnvShapeIndex()`), a point count and 64 points each, as
  a source's shape.
  Once released, new fields go at the end of their section's
  enum and fields are never reordered. The same goes for the wave, filter type, effect type, source type, route
  mode, slot aux field, split field, wire field, port, amp model, cabinet, mic, fixed Mult, operator sync (`OpSync`),
  warp (`OpWarp`), unison curve (`UniCurve`), unison pan (`UniPan`), morph mode (`MorphMode`) and the sequencer's play
  mode, rate, order and scale (`SeqPlay`, `SeqRate`, `SeqOrder`, `SeqScale`) and curve (`SeqCurve`) lists, keyboard
  scaling's curve (`KeyCurve`), the routing views and the operator page's tabs,
  which are stored by number. `src/params.h` pins each section's first and last ids, the counts they come from and
  the stored lists' members with static_asserts, so a change that renumbers them doesn't compile (and
  `src/params.cpp` checks that every param's id leads back to it: a check that costs MSVC seconds goes in a `.cpp`,
  run once, rather than in a header every file pays it in); a field added to
  `FxParam` also needs its effect type in `kFxFieldRuns`. A retired type keeps its number and fields: the
  three-band compressor (`FxType::MultiComp`) is no longer offered, and `readParams()` loads a slot holding one in a
  state from before version 6 as a three-band dynamics set from its fields, moving the modulation aimed at them
  along. A host can still set the retired type (the Type param's range takes it in): such a slot does nothing, and a
  state keeps it as it is. Likewise the LFO's Random shape (`LfoShape::Random`), which the Random source took over:
  the shape list doesn't offer it, a state from before version 11 loads an LFO with it as a Random source, and an LFO
  a later state holds with it plays it as a Random source does; and the LFO's Custom shape (`LfoShape::Custom`),
  which the Curve source took over, a state from before version 12 loading an LFO with it as a Curve source.
- Like an effect slot, each of the 32 source places has the fields of every type (an LFO's, an envelope's, Slew, CC,
  Bipolar and Input for Random and MIDI, and Curve's Sustain, loop region and Slew), and a Type param (Empty, LFO, Envelope,
  Random, MIDI or Curve) that picks which are used (`sourceFieldUsedBy()`). A Random source is a random LFO: it runs
  through the LFOs' code with the LFO's clock fields, Smooth and Chaos, as an LFO of the retired Random shape
  (`lfoShape()`), its polarity its own Bipolar. A Curve source is an LFO of a drawn shape: it runs as an LFO of the
  retired Custom shape, with every LFO field but Shape (its polarity the LFO's Unipolar, which the editor turns on for
  a new one) and its place's freeform shape, and two fields of its own after Anchor. Its loop region, `LoopStart` and
  `LoopEnd` (along the curve, the whole curve to begin with; `ShapePoint`s, so hidden and never modulated; in order
  whichever way a state holds them, `lfoLoop()`), is what Retrigger, Free, Anchor and Sustain loop (`loopPhase()`,
  `anchoredPhase()`), Rate staying the whole curve's. `CurveSustain` is a fifth clock mode (`LfoClock::Sustain`,
  `lfoSustain()`), winning over Free, Anchor and One-Shot: it loops while the key is down and, let go
  (`Synth::releaseVoice()` marks the voice's LFOs, and its next block's `leaveLoop()` acts on it), leaves the loop
  from the level it is at, an offset on top of the curve fading by the next point (or over 5 ms, `kCurveGlide`), on
  to the curve's end; `Synth::modulationReleasing()` keeps the voice until then, as it does for a releasing mod
  envelope. Its `CurveSlew`, after them (Off at 0, `Synth::curveSlew()`), lags what it puts out behind its curve as a
  MIDI source's Slew does. Hosts are given only a place's slot depths and Amounts; its fields go by place
  ("Mod 3 Rate"), since a name can't follow the type. A MIDI source takes CLAP MIDI events
  (controllers, channel and polyphonic aftertouch) and CLAP's pressure and brightness note expressions, as its Input
  picks.
- Hosts are given 2,057 of the 52,172 params (`hostParamCount()`, `hostParamIndex()`, `hostListed()` in
  `src/params.h`), 1,600 of them the slots' depths and Amounts. The editor's alone are those the table marks
  `CLAP_PARAM_IS_HIDDEN` (the drawn shapes and curves, the operators' drawn envelopes' fields, the Custom waves' partials and counts, the chain's retired order, the
  feedback's route modes and the retired fields), every slot's target, aux, curve and bypass, the effect slots'
  fields, the effects graph (its splits, joins and wires), the source places' own fields (a place's slot
  depths are listed, whatever it holds) and the sequencer's root, scale, patterns' lengths and loops, notes, their bends and curves. Nothing hosts
  see changes with an effect's or a source's type: the CLAP and VST3 plugins hide none of their params. The remote
  pages follow the sources and the operators switched fixed. CLAP's `count()`/`get_info()` and VST3's
  `getParameterCount()`/`getParameterInfo()` index that list; a value sent for any known id is still taken. The
  state, presets, undo and the editor's modulation cover every param. The core tells hosts of edits to listed ones
  only, and marks the project dirty for the others.
- The sequencer's Swing, Gate, Velocity and Humanize, and its pattern picked (in whole steps, `modulatedInSteps()`),
  (`isSeqFeel()` in `src/params.h`) are modulation targets past
  `kModulatedCount`, the params the voices and the effects read: the synth hands them to the sequencer before each
  stretch it plans (`Synth::feedSeqFeel()`) as set with the host's and the macros' modulation and the newest voice's
  slots, or the last one's as the effects ring out, and the telemetry's `seqFeel` says how far it took them. A host
  modulates them for every note at once.
- A slot's *target* param stores the target's id + 1 (0 = unused). Each slot also has a depth, an aux (the
  source + 1 that scales it, 0 = none) and an Amount (0–1, 1 to begin with), whose defaults leave a slot as it was.
- Sources are numbered two ways. In memory the 32 places come first (`kMaxSources`), then the macros, the note source,
  the mod wheel, velocity, the pitch bend and the operators. States and presets store a source by the number it had
  when there were 16 places (`kFirstSources`), the places added later after the operators (`storedSource()`,
  `sourceOfStored()` in `src/params.h`): an aux's value (`writePatch()` and `readParams()` turn it), the MODS view's
  empty modulations, and the ids of the slots' auxes, Amounts, curves and aux depths. So a project from before keeps
  its sources. The layout's source order and folded cards keep the first 16 places' where they were and put the rest at
  the chunk's end.
  Depths and Amounts are automatable params; targets and auxes are wiring, set in the editor and not given to
  hosts, but saved like any other param. A slot swings its target one way or both as its source's
  own switch says.
- Edits in the editor reach the host as `PARAM_GESTURE_BEGIN` / `PARAM_VALUE` / `PARAM_GESTURE_END` events (in
  VST3, `beginEdit()` / `performEdit()` / `endEdit()` on the main thread). Hosts
  record automation and undo from these, and mark the project as changed. Loading a preset and Init send none: the host is asked to read every value again (CLAP's values rescan,
  VST3's kParamValuesChanged) and told the project changed, so it records none of it as automation or in its undo.
  The queue they wait in keeps room for the end of each gesture begun, so a host is
  never left inside one (a gesture whose begin didn't fit goes without its end); edits that don't fit have the host
  read every value again instead (one values rescan, from the main-thread callback). For half a second after the plugin
  tells the host a param's value (or a preset load changes it), it takes no value for that param from the host: Bitwig hands values back a
  little later, and taking them would flip a preset loaded meanwhile back to the one before, for a moment. VST3 hosts
  hand every edit back to the processor, which the same moment keeps out.
- The state version rises at most once between two tagged releases. A change that needs older states loaded
  differently goes into the step already made since the last tag, if there is one (its comment in `src/state.cpp`
  and the checks that load a state from before it), rather than adding another. States saved by a build between two
  tags are not kept loading as they were: only released versions are.
- The state blob holds `(id, value)` pairs with a version number (currently 17: version 1's Volume loads 12 dB
  lower, since Volume now gives back the 12 dB the voices go into the effects with; before version 3, a state in
  poly loads Legato off, since it did nothing there then and acts there now; before version 4, a modulation's depth on an
  envelope's attack, decay or release loads scaled by `kMaxEnvNorm / (kMaxEnvNorm - kMinEnvNorm)`, since their
  range now reaches below 0 down to 0 ms and a depth is a share of its target's range, and before version 3 (up to 0.3,
  whose envelope times and holds reached 0 to 1) divided by `kMaxEnvNorm` on top, a depth on a hold too; before version 5, an
  operator's ratio loads from its Coarse and Fine, and their modulations move onto Ratio; before version 6, a slot
  holding the retired three-band compressor loads as a three-band dynamics, while a later state loads one back as
  saved, so a state always loads what was saved; before version 7, a modulation's depth on Glide loads scaled by
  `1 / kMaxGlideNorm`, since its range now stops at 6 s; before version 8, the chain's order loads from the retired
  `RetiredFxOrder`, the rank of the first 16 slots' order, onto its places, a param a place, since 32! wouldn't fit a
  double; before version 9, a state that doesn't wire the effects graph's output, saved with the effects as a chain,
  loads its chain's order as the graph's line (`fxChainWires()`), since the wires alone put the effects in order now,
  and the places do nothing; before version 10, a modulation's depth on a three-band dynamics' Above or Below threshold
  loads scaled by `kMdRetiredThresholdMinDb / kMdThresholdMinDb`, since their range now reaches down to -80 dB rather
  than -60; before version 11, a Random source, which then took a value for each note, loads unsynced at 0 Hz (the
  LFO fields it now uses otherwise at their defaults), standing still and taking one for each note, and an LFO with
  the retired Random shape as a Random source running as it ran (`loadRandomSources()`), and an LFO at Rate 0, then its
  slowest (0.02 Hz) and now standing still, at `kLeastNorm`, as slowly as it played; before version 12, an LFO
  with the retired Custom shape loads as a Curve source running as it ran (`loadCurveSources()`); before version 13,
  a modulation's depth on an operator's Unison Detune loads scaled by `kRetiredMaxUnisonDetune / kMaxUnisonDetune`,
  since its range now reaches 200 cents rather than 100, and its Unison Pan, Alternate to begin with, loads Ramp, as
  its copies played, and a modulation's depth on a source's Division loads scaled by
  `kRetiredSourceDivisionSteps / kSourceDivisionSteps`, since it now reaches two note values slower, 8/1T and 8/1,
  numbered -1 and -2 (`kSlowestDivision`) so that its stored values keep their note values; before version 14, a
  pattern's bend and mod wheel curves, which held their first and last points' values and now wrap around the pattern's
  end, load with points at the pattern's start and end that hold them (`holdSeqCurveEnds()`); before version 15, an
  operator's Warp Amount on a Sync warp and the depths on it load scaled by
  `(kRetiredMaxSyncWarp - 1) / (kMaxSyncWarp - 1)`, since it now plays its wave up to 16 times a cycle rather than 8;
  version 16 has 32 source places rather than 16, and loads an earlier state as saved, its sources stored by the numbers
  they had, the version rising so that an earlier build refuses a state whose later places it would leave out; before
  version 17, a sequencer playing from a key (Key or Latch) with its Root off C4 loads that difference into Transpose,
  and Octave what Transpose can't hold (`transposeFromRoot()`), since the keys now transpose a pattern from C4
  (`kSeqTransposeFrom`) and Root sets the scale alone;
  version 17's sequencer notes may also bend, an earlier state loading with none, and its Play is Key to begin with, an
  earlier state without one loading Host, its default then),
  followed by the editor layout:
  node, filter and shaper positions, selected node, zoom, the routing view, a retired field (once whether the modulation panel was collapsed, which it no longer is),
  the order the sources show in, the routes kept at 0 %, the shape editor's grid, the macros' names, which
  effect and source cards are folded, whether the keyboard strip is collapsed, the operator page's tab, the
  scope's cycle count, the wires bent away from their first bow (by route), the MODS view's empty modulations
  (each its source + 1 and its target's id + 1, 0 for none), how shapes are edited (Steps, then Snap), which
  effect cards past the 16th slot are folded, the node the effects graph's inspector shows, whether the FX view shows
  the graph, the band each split shows and which splits' containers are folded, the SEQ view's roll (whether it
  draws, its snap, its lane, its lowest row and whether it is folded to the scale), and the tempo the patch was last
  saved as a preset at (0 for none), which the synth's own tempo and a host that lets the editor set the tempo
  (`Controller::hasTempo()`) take up as the preset loads, then what the preset says about itself (`gui::PresetInfo`: its author, its category by
  name and its one-line description, each as its length and its bytes in UTF-8), which the library's scan reads past
  the params (`readPresetInfo()`) and an older plugin, reading a longer layout than it knows, skips, then the
  source places past the first 16 (their order and folds) and whether the SEQ view's roll bends notes (its Bend tool).
  The host's state goes on with
  the settings chunk (the oversampling, MPE, MPE's bend range, whether the synth follows the host's tempo and its
  own tempo, `Settings` in `src/state.h`) and a preset chunk (whether the patch changed since its preset, then the preset file's path
  in UTF-8, left out past 64 KB); preset files stop after the layout. Params and layout fields a
  state doesn't have keep their defaults; a layout or settings chunk longer than the loader knows (up to 4096
  floats are read) keeps the fields it knows and is skipped past, so the chunks after it still load; a chunk cut
  short keeps the floats that arrived whole, and a layout value that isn't a number keeps its default. States
  with a newer version are refused.
- Times and LFO rates are stored as 0–1 on a log curve (1 ms–20 s, 0.02–100 Hz), so host knobs and modulation
  have resolution where it matters. A rate's 0 (an LFO's, a Random source's, a phaser's, flanger's or chorus's) is
  0 Hz, standing still (`sweepRateHz()`). The value text shows ms/s/Hz. Envelopes' attacks, holds, decays and releases
  stop at 10 s (`kMaxEnvTime`), their range ending short of the curve's top; a state or preset holding a longer one
  loads it as 10 s, since loading holds every param within its range. Their attacks, decays and releases also
  reach below 0, down to `kMinEnvNorm`: a straight line from 1 ms at 0 down to 0 ms (`envTimeFromNorm()`), so
  stored values from 0 up keep their meaning.
- The plugin takes CLAP note events (preferred), raw MIDI and MPE MIDI (see `Synth::setMpe()`; MPE is a setting
  in the settings dialog, not a param, so presets leave it alone: with it on, channels 2 to 16 bend, press and slide
  their own notes, kept per channel in the synth), and reports
  `NOTE_END` so hosts with
  polyphonic modulation (Bitwig) know when a voice has finished. In mono, a note that is taken over keeps its
  host voice while its key is down and reports `NOTE_END` when the key is let go. Per-note modulation follows the
  note that is sounding. MIDI CC 1 also moves the Mod Wheel param (`PluginCore::playModWheel()`), which the audio thread
  reports to the host in its place in the block (CLAP's output events, VST3's output param changes), as a hardware mod
  wheel turning the wheel in the editor's keyboard strip.
- Voices are allocated when the host activates the plugin, for the current Voices value; the audio thread never
  allocates. Where there isn't the memory, the activate fails (CLAP's `activate` returns false, VST3's `setActive()`
  `kOutOfMemory`), and the plugin plays silence until one goes through. Lowering the limit takes effect at once: the voices past it fade out as stolen ones do. Raising it past what is allocated calls
  `host->request_restart()` (in VST3, `restartComponent(kLatencyChanged)`, on the main thread), and the host then
  deactivates and reactivates the plugin (a host that doesn't is asked again once more voices or another
  oversampling factor need one). So does an edit or a state that moves the latency (`PluginCore::latency()`: the
  voices' decimator's delay and the effects' latest path's, see `src/fxlag.h`), which the plugin reports to hosts as
  of its last activate (CLAP's latency extension, telling the host's `changed()` from activate, and VST3's
  `getLatencySamples()`), but not while a key or a sustain pedal holds a note (`Synth::holdsKeys()`, which
  `endBlock()` keeps in `keysHeld_`): a key let go while the plugin is inactive would reach no voice, so the audio
  thread has the main-thread callback ask once they are all up, and an activate keeps the voices only while none is
  held. A voice limit still settling holds every restart off, so they come as one. What VST3 only allows on the main
  thread, and the audio thread may ask for, goes through a message-only window there (on Linux, a pipe the host's
  run loop watches: the view's frame's, else the one the host application offers as `initialize()`'s context, else,
  with neither, a thread of the plugin's own, which takes a lock the host's main-thread calls take too, so it never
  runs beside them, and calls `restartComponent()` without it). Each voice takes about 86 KB at 48 kHz, mostly the two comb filters' delay lines, which grow with the oversampling: at the default
  2x, about 125 KB, so 512 voices reserve about 64 MB. Voices isn't automatable for the same reason. The comb lines,
  and the effects' delay lines (every slot has every type's, a few MB a slot), are `ZeroBuffer`s
  (`src/zerobuffer.h`): pages Windows hands out as zeros once they are first written (on Linux, `calloc()`'s, whose
  large blocks glibc maps as zero pages too), so the lines no voice or effect uses take address space, not memory, and
  allocating them fills nothing.
- The CRT is linked statically. The `.clap` and the `.vst3`'s DLL depend only on Windows system DLLs (kernel32,
  user32, Direct2D, DirectWrite, and ole32 and shell32 for the preset file dialogs), not on the VC++ redistributable. The `.vst3` is a
  bundle: `FMVibe.vst3\Contents\x86_64-win\FMVibe.vst3`, on Linux `FMVibe.vst3/Contents/x86_64-linux/FMVibe.so`.
  On Linux, libstdc++ and libgcc are linked statically likewise: the plugins depend on glibc, X11, Cairo and
  fontconfig.

## Updating the CLAP and VST3 SDKs

Change the tag in the `FetchContent_Declare(clap ...)` URL in `CMakeLists.txt` and replace the hash with the new
archive's SHA-256 (`Get-FileHash <file> -Algorithm SHA256`). The VST3 SDK comes as three archives of the same tag:
`vst3_pluginterfaces`, which the plugin is built against, and `vst3_base` and `vst3_public_sdk`, which only the
validator is built from; update the three together. Its interfaces' version also goes into the class info hosts
see.

## Next steps

- Presets: the `clap.preset-load` extension and preset discovery, so hosts can browse FMVibe's presets too.
- Sound: oversampling for high-index patches.
- AU and standalone builds: [clap-wrapper](https://github.com/free-audio/clap-wrapper) builds these from the
  CLAP plugin. The plugins' editor runs on Windows and Linux (X11), not yet on macOS (the browser demo has its own
  drawing backend).
- VST3: a `moduleinfo.json` in the Windows bundle, as the Linux one has, for hosts that scan with it.
