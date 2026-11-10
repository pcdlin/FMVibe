# AGENTS.md

Notes for anyone (human or agent) working on FMVibe, a six-operator FM synthesizer as a CLAP and a VST3 instrument
plugin for Windows. The README introduces the plugin and says how to build and install it. What the plugin does and how
its editor works, in full, is in [docs/features.md](docs/features.md): read the parts you need before changing a
feature.

## Ground rules

- Parameter ids, the wave, filter type, effect type, source type, amp model, cabinet, mic and fixed Mult lists, and
  the state layout are
  stored in host projects and preset files: follow the rules under [Plugin facts](#plugin-facts) when touching them.
- Keep `ctest` green (the CLAP and VST3 smoke tests, audio quality, UI snapshots). New editor interactions get a
  replay in `tools/uishot.cpp`, new engine behavior a check in `tests/smoke_host.cpp` or `tests/quality.cpp`, and
  what only the VST3 plugin does a check in `tests/vst3_host.cpp`.
- Both plugins sit on one core, `PluginCore` (`src/plugincore.cpp`): the params, the state, the editor's
  `Controller`, presets and the editor window. What one format does goes in its own file (`src/plugin.cpp` for CLAP,
  `src/vst3plugin.cpp` for VST3), and what both do in the core, which tells each host through its `host...()` hooks.
- When a feature changes, update [docs/features.md](docs/features.md); keep `README.md` to its short
  description and the build and install instructions.
- Don't start Bitwig or another host yourself: the plugin is tried in the host by hand.
- The editor draws through `Canvas`, which has three backends: Direct2D (`src/gui/canvas.cpp`), the browser
  demo's 2D context (`src/gui/canvas_web.cpp` with `web/site/canvas2d.js`) and Cairo for the Linux development
  build (`src/gui/canvas_cairo.cpp`). A new drawing call needs all three.
- The editor's corners and text sizes keep to the scales in `src/gui/theme.h`: a radius is `kRadiusMark`,
  `kRadiusControl`, `kRadiusCard` or `kRadiusPanel` (a ring around something takes its radius plus how far out it is),
  and a text size one of the `kText...` sizes. New drawing uses them rather than literals.
- Text the plugin shows (labels, tooltips, menus, messages, parameter names, remote control pages) is worded in
  `src/uistrings.inc` and looked up by name: `tr(Str::Name)`, or `trf()` for a wording with `{1}`, `{2}` in it. New
  text gets a line there. Tests that match on text (menu entries, page names) look it up the same way, so a
  rewording keeps them green.

## Build

What you need, `build.ps1` (which runs the tests below with every build, unless `-SkipTests` builds only the plugins),
the plain CMake commands and installing into a host are in the [README](README.md#build).

## Testing

`ctest` runs four programs:

- **Smoke test** (`tests/smoke_host.cpp`): a small headless host. (It and the VST3 test set `FMVIBE_SETTINGS_DIR`,
  which `src/usersettings.cpp` reads in place of the roaming app data, so their editor resizes don't land in your
  settings file.) It loads the `.clap` and checks the parameter
  table, text round-trips (at each end and just inside it too, the slowest sweep rate and the shortest time typed
  neither still nor Off), the note source's range (every MIDI note, on to the pitch at 20 kHz) and remote-control pages. It then plays
  notes, checking:
  - hosts given the params they automate and see and the slots' depths and Amounts, and not the effects, the
    sources' own fields, the slots' wiring or the editor's drawn shapes (whose values still read back by id); no
    effect getting a remote-control page, a page per source, and the pages pointing only at params hosts are given;
  - sound comes out and `NOTE_END` arrives after the release;
  - voice stealing and choke, and a reset ending the notes playing, their ends reported in the next block;
  - per-voice host modulation, and a mod-envelope slot driving its target, and running its release out after
    the operators have faded (the voice reporting `NOTE_END` only then);
  - spectrum checks on an A4: the matrix produces FM sidebands; a Ring route at 100 % leaves only the sum and
    difference and at 50 % is AM, Linear FM keeps the mean pitch (through zero too) where Exp FM lifts it by its
    octaves, and feedback stays Phase whatever its mode param says; every wave sounds; a saw's 2nd harmonic sits at
    half the fundamental, and Alt Sine's, Organ's and Vowel's where their series put them; a Custom wave plays its
    partials, only as many as its count (the 40th silent at 32, heard at 64); a low-pass at 300 Hz and a high-pass
    at 2 kHz move that ratio the right way, each slope from 6 to 24 dB harder than the one before (the 6 dB
    low-pass a one-pole), and a band-pass set to 6 or 18 dB plays 12 or 24;
  - every filter type stays finite at high resonance at every slope, and a voice ringing through a resonant comb
    still ends;
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
  - the mod wheel moves nothing until it is assigned, and then its target, like a macro;
  - a MIDI source following its controller (and not another), on pressure the channel's and a note's own
    (polyphonic aftertouch or CLAP's pressure expression, not another key's), and on slide resting in the middle and
    following a note's brightness expression; Slew taking a controller turned up mid-note slowly, and a Random source
    giving each note a level of its own;
  - MPE over MIDI, switched through the state's settings chunk (which saves it): with it on, a member channel's
    pitch bend bending its note alone and the master channel's every
    note, and a member channel's pressure being its note's alone (the master channel's standing in until it sends
    some), in mono too for a note taking the voice over; with it off, any channel's bend bending every note;
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
  - each effect on its own, among them a shimmer's tail rising in octaves where a hall's has none (a fifth up or
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
  - an operator's Pan placing it, equal-power, through a filter too, and a modulator's leaving what it modulates
    alone;
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
  - a lower voice limit applies immediately, while a higher one requests a restart and is reported through
    voice-info after reactivation, and a state with a higher one, loaded while active, requests one too.

  It opens the editor inside a hidden host window and drags the Volume knob with mouse messages. It checks that
  the param changes and that the host receives gesture-begin, value and gesture-end events, and that an older
  value the host hands back right after isn't taken; and that an editor sized at the system's DPI and given a window
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
  which loads as the defaults), and checks that
  every remote control page has an id of its own. It also reports CPU cost: 16 voices × 6 operators take about a third of what they took
  before the engine's per-block route lists and shared params, which on the development machine was about 22 % of
  one core at the default 2x oversampling and about 38 % with every operator at 8 unison copies (now roughly 7–8 %
  and 25 %; the smoke test prints the figures).
- **Audio quality** (`tests/quality.cpp`): drives the engine directly and measures, at every oversampling factor:
  - the decimator's passband ripple and alias rejection;
  - aliasing (the energy off the harmonics, 20 Hz–20 kHz) of a high saw, bright FM (phase, linear and ring),
    feedback and a driven filter;
  - zipper noise from an LFO on an operator's level and Pan, a matrix amount, a filter cutoff and a wire of the
    effects graph;
  - clicks when a voice is stolen (and six at once by a chord), when a note joins a held one in poly legato
    (fading in over 5 ms, its Random LFOs carrying on with the held one's step), and when a param (Volume, and an effect's: a compressor's Makeup, a delay's and a
    reverb's Mix, a DJ EQ band, a three-band dynamics' Output, an amp's Output, Mix and Gain, a utility's Gain (0 to
    +12 dB), Pan and Width, a wire's Level) jumps while events split the block into single frames,
    when a Param EQ band is switched on and off, when a utility's Mono Bass is turned on from Off and off again, and
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
    one amp on both sides' sum in mono and one a side in stereo, Mix at 0 leaving the sound alone, the Fuzz putting out
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
    from 60 dB to 80, a version 10 state's as saved;
  - the reverb's tails: every algorithm's late spectrum without lasting resonances and its two sides unalike, an
    undamped tail falling 60 dB around 1 kHz in its decay time, and a reverb with a 200 ms pre-delay ringing on
    until its tail has been heard, and stopping after; a sample that isn't a number, and one louder than any sound,
    going into a delay and a reverb leaving them sane, ringing out and playing on; a delay switched off and on again playing back nothing it
    held, its lines part written or written all round; a compressor running its release out in silence, a quiet
    sine after it untouched; and effects that have rung out costing little for silence, which comes out as silence;
  - a shimmer's tail after an A4, with Shimmer half and full up: falling 60 dB in about its decay time, its energy
    on the octaves and the first two octaves up narrow lines (not smeared by the shifter's crossfades); and at the
    longest decay, undamped and full up, still dying away; and Shimmer turned down to 0 in short chunks and up again
    once the reverb has rung out bringing back nothing of the tail before;
  - a shaper effect switched off, its drawn curve changed and on again playing the new curve from its start;
  - a drawn shaper curve's points moved gliding in rather than clicking, in whole blocks and in single frames, and
    dragged up and down putting out no DC, made symmetric too;
  - a quiet, slow sine through a drawn shaper curve coming out as drawn, without spikes where it crosses the
    curve's table steps; a symmetric drawn curve that doesn't pass through the middle putting out silence for
    silence, and an input that isn't a number reading inside its table;
  - the filter effect switched to another type or slope and back, and a plate switched to a hall and back,
    playing nothing their stages held before; and the effects running on until a shaper's DC blocker has settled;
  - the phaser's and flanger's polarity: negative moving their notches to where the positive peaks were, and
    turned over mid-sine fading through nothing rather than clicking;
  - the random LFO's steps and glides, a freeform LFO's points at one time (a vertical line, its last
    counting, and a point at the cycle's end holding a one-shot there), the phaser's and flanger's sweeps synced to the host tempo, the parametric
    EQ's, the phase disperser's and the voice filters' analyzers (falling away as fast at 96 kHz as at 48), and that each example preset holds valid values
    and sounds at a sane level;
  - each factory preset's file being sparse, as the preset tool writes it;
  - the mod wheel leaving the FM alone until it is assigned, and the output meter's peaks, fall and clip light;
  - Volume setting the level after the effects, and a mod envelope's level half way through its release;
  - every note reporting its end once: in mono a note taken over in its release, with note ids a note on a key
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
  - an operator's Phase starting its wave that far into its cycle, set or modulated;
  - an anchored LFO standing where the transport puts it, from its Phase, by beats when synced and by seconds when
    not (a one-shot not anchored), following it through a block split into pieces, running on from there while the
    transport is stopped, and a random one stepping as the transport passes a cycle;
  - the scope capturing nothing until asked, then an operator's cycles from where they start (a sine's a sine, a
    square's a square), standing still from one span to the next under FM, two cycles differing under a modulator
    at half the ratio, a slow span shown as it fills, nothing again once it isn't asked for, and a high operator's
    (a cycle of 4.5 samples) for little more than the voice costs;
  - the header's output scope capturing nothing until the synth analyzes for the editor, then two periods of the
    note, standing still from one span to the next: a sine, a wave crossing 0 many times a period, one repeating
    every other period and a high note's over more periods; falling silent with the output, and nothing once
    switched off;
  - the CPU cost of each factor.
- **UI snapshots** (`tools/uishot.cpp`): renders the editor offscreen to PNGs in `build\<config>\ui-shots`. On the
  way it replays drags and clicks:
  - the effects graph's slot grid: switching to it showing the chain as its main line, as wired; the main
    line's + opening the list of effects with Add 2-band split after them, the split coming with its join, its
    empty bands each curving out to a + on its row, each + ringed in its wire's color (read off the rendered
    pixels); a band's + putting an effect on its band; the join's node showing
    the split's card (at the inspector's right), its High knob setting the high band's level, in dB, and its S
    soloing a band, the split and join nodes marked with an S and the other band's node and wires faint, and the
    main line's wires grey, each band's and each half of the split's and join's nodes in its color, a split's coming
    in from grey at its left and a join's going out to grey at its right (read off the rendered pixels), and then
    the other band's too,
    both soloed; the split's crossover handle dragged on its card, and its slope picked from its list there, 18 dB refused; its title's
    2 | 3 switch splitting it in three, an empty middle band between shown, into the join's third input, its high
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
  - the output meter and scope moving repainting the header alone, a key the keyboard alone and an operator's scope
    the inspector alone, each painted over the frame before coming out as a whole paint does, pixel for pixel, and
    an LFO moving repainting the whole editor;
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
    showing the high band, whose dashed card opens the list of effects (no split) and puts a reverb on it; the
    container's Low knob setting the low band's level, lowering that band's curve on its display (read off the
    rendered pixels), its S buttons soloing each band on its own, the band shown faint while the other is soloed (read
    off the rendered pixels), both undone, and its crossover handle dragged; the
    graph and back leaving every wire as it was; the container folded to a strip and unfolded; dragged by its title
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
    the bus, with nothing going in, and stopping by its arrowhead dragged away; routing a carrier through a filter,
    which goes out in its place, and picking waves and filter types;
  - moving routes by dragging their arrowheads onto another node and onto the OUT bus, with their modulation, and
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
  - a MODS row's depth bar ticking where its modulation is now: an LFO's under a playing note (none with no note),
    a macro's from its knob (read off the rendered pixels);
  - a node switched on where another one shown sits coming back clear of it, undoably, and one whose place is free
    staying there;
  - a routing node under the mouse showing an x in a circle at its upper right (a filter's at its box's corner, read
    off the rendered pixels), whose click switches it off, undoably;
  - the MODS view's filter: its magnifier opening it to take the keyboard, the words typed (every one, in any case)
    narrowing the rows, its × emptying it, Escape and a click elsewhere letting the keyboard go; and the target
    menu's search listing the targets found with their path, the first that can be picked lit (read off the rendered
    pixels), Up and Down picking one, Enter taking it, Escape emptying it and then closing the menu; an operator's
    targets in two columns, the envelope's beside the pitch's;
  - the modulation panel's + SOURCE opening the list of source types under it, adding Random and MIDI sources, each
    with its card, a MIDI card's knob picking
    its controller and its input's list pressure or slide instead, its CC knob going, and the list button's tooltip
    following its input;
  - adding an empty modulation in the MODS tab: it opens its source's menu, takes a source and modulates nothing,
    then a target from Global (which offers no Mod Wheel) makes it its source's modulation, each pick undoable, and
    its × removes it; one given a source that has rows going among them, its source menu, open over the rows,
    lighting none of them (read off the rendered pixels); a source card's button showing its modulations in MODS, or with none adding an empty one
    from it;
  - a wire label under the mouse opening into a pill (read off the rendered pixels): its mode part's list picking
    the route's mode and the wheel stepping it, undoably, its × removing the route (and an OUT label's its node's
    output); resting, a label showing a mode other than Phase (read off the rendered pixels); the mode also picked
    from its label's and its matrix cell's
    right-click menus (the cell's corner showing the glyph, read off the rendered pixels) and from the operator
    page's INPUTS tab, whose label sets the depth, each an undo step; a route moved onto another operator keeping
    its mode, and one disconnected starting as Phase again;
  - bending a wire by dragging it sideways (lit under the mouse, its label following its middle), either way,
    undoably, back to its first bow by a double-click or Reset Bend, its bend going with it when it is
    disconnected, and one along the panel's top bowing only as far as keeps it and its label inside (read off the
    rendered pixels);
  - an operator lighting up while modulation opens its route from 0 %, and not with the glow setting off (read off
    the rendered pixels), and modulation dropped on a silent operator's OUT cell in the matrix showing its wire to OUT;
  - an operator's Pan knob turning from its middle and reset to it by a double-click;
  - the node the inspector shows filled and ringed in the graph, picking another moving both, and a switched-off
    one's ghost filled while it is shown (read off the rendered pixels);
  - a switched-off source's handle going faint, and an LFO's, an envelope's, a macro's and the note source's until
    they modulate something
    (read off the rendered pixels);
  - the dot on a filter's and a shaper's tab while it sounds out, the output meter and its clip light, and the
    output scope beside it, empty with nothing captured and drawing a span at its peak (read off the rendered
    pixels), its height easing down to a quieter span, and its spans too small to draw painting nothing again;
  - a shaper: switching it on with its tab, sending an operator and a filter into it (in the graph and the matrix)
    and running it onto an operator and into a filter, its page's curve list, link switch, symmetric switch (for
    a Custom curve) and knobs, its Custom curve drawn on the page and in the big editor;
  - the shaper effect's SYM switch, hidden but for a Custom curve, putting a point in the middle of its drawn curve, a double-click on the curve's
    mirrored left half adding a point on the right, the middle point refusing to be deleted, and the switch
    undoing with its point;
  - dragging the filter response handle, switching between the graph and matrix views (the tab shown opening its
    panel's edge, and a hovered one's shape, read off the rendered pixels), and toggling a matrix cell: undone, its
    route (or one the wheel switched on) not kept at 0 %, and a press drifting a pixel setting nothing; a shaper's
    row routing it into the other shaper and, on the diagonal, into itself;
  - picking a filter's slope from its list, a band-pass's 6 and 18 dB refused;
  - Points | Steps and Snap in the other big shape editors: Steps drawing a step in the note source's curve with a
    plain click, and Snap snapping a shaper effect's curve point in its editor;
  - making sources unipolar and bipolar from their menus (an envelope's and an LFO's card, a macro's knob and
    handle, the mod wheel and the note source's display);
  - dragging a Param EQ band's handle, and Alt-dragging it for its Q; its bands' chips: a click on one, its shape
    or a band's handle picking the band its knobs set, its power symbol and its number turning the band on and off,
    a switched-off band's handle pressed switching it on, undoably, and
    its shape's list (the third band's inside the card) picking the shape, a notch's handle moving only across, and
    its Amount knob setting the whole EQ's amount;
  - dragging an envelope's handles, past the display's end too, where the envelope zooms out to stay inside (read
    off the rendered pixels), and the attack's below 1 ms, to 0.5 ms and to 0 ms, drawn as a vertical line (read off
    the rendered pixels), and straightening a segment's bend to 0 % with a double-click, its readout drawn
    over the curve (read off the rendered pixels);
  - an operator page's ENVELOPE, UNISON and SCOPE tabs: a line across the envelope at the newest voice's level,
    and a dot on the curve along the attack, decay and release (one releasing from above the sustain too), as on a
    mod envelope's card (read off the rendered pixels), the unison knobs on theirs, the scope asking for its
    operator's output (and a noise operator's, or a hidden one's, for none), its trace drawn over 2 and 4 cycles
    and as a slow span fills, the faint plain wave with no note, a hidden tab's dot while one of its knobs is
    modulated (read off the rendered pixels), and a dragged handle opening the tab it is held over;
  - an operator's Ratio readout: each digit dragged by its own place, carrying and held to 64.00, each drag an undo
    step; the wheel stepping the digit under the mouse, a double-click rounding to it, its depth handle dragging an octave every 60 px, the hovered digit lit (read off
    the rendered pixels), a typed fraction, and a source's handle dropped on it modulating it, its reach drawn along
    the track, a straight line to 64 with its tick at the end there and an arrow for modulation going on past it
    (read off the rendered pixels);
  - an operator's fixed pitch switched on by its keyboard (and by the wheel over it), Freq and Mult set to where the
    ratio was in one undo step and taking Ratio's and Fine's places, Mult stepping (and typed into, its × taken
    whole), and Pitch and Detune dimmed (read off the rendered pixels), not turning and not ringed for a dragged
    handle (read off the rendered pixels);
  - switching operators and filters on and off with their tabs, the matrix's names and their ghosts in the graph,
    and off from a node's right-click menu, a switched-off one's page dimmed (read off the rendered pixels);
  - the play switches and the glide knob;
  - an LFO starting synced in quarter notes, and its sync switch freeing it; its Phase knob, marked on its display
    and faint while the LFO is free, and lit while it is anchored (read off the rendered pixels); its clock modes
    going one at a time, undoably, a click on the one lit leaving it and the wheel stepping along them, and its
    Unipolar switch standing apart;
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
  - the utility's card: added last from the list of effects, leaving the sound as it is (Mono Bass Off); the handles
    on its display setting Mono Bass, Pan, Width and Gain, each drag an undo step, the band narrowing into its middle
    below Mono Bass's line (read off the rendered pixels), and a double-click putting Mono Bass's back to Off;
  - the newer effects' cards (the widener's knobs, the frequency shifter's Shift, the phaser's SYNC, the
    flanger's STEREO and its + | − picking its polarity, the filter's Key Track and the shaper's and delay's LINK
    too) and the list of effects with their icons;
  - a filter's and the filter effect's responses moving with the note their key tracking follows (read off the
    rendered pixels);
  - the disperser's and widener's displays following their params as modulation takes them (read off the rendered
    pixels);
  - switching effects with their cards' power switches and icons (a switched-off one's staying grey under the mouse,
    read off the rendered pixels), dragging cards by them, and folding cards, the icon staying put, one in a slot past the 16th
    too;
  - the right-click menu on knobs;
  - the macros' knobs (a double-click setting a bipolar one to its middle), handles and names, the note source's
    and velocity's handles, displays and curve editors, velocity's handle faint until it modulates something (read
    off the rendered pixels), and the mod wheel's
    handle (new modulation starting upward); with no note playing, a knob's dot showing where a macro takes it
    (read off the rendered pixels); the octave shift's small buttons under the keys; collapsing the keyboard strip
    to its top row by its chevron or title, the mod wheel's handle moving into it and still mapping the wheel, and
    Bend's knob, over the bend wheel, staying in it and turning;
  - the dashed cards that add effects and sources (opening the list of types at the mouse, the effects' moved back
    inside the editor at the chain's right end and staying over a switched-off card's dimming, read off the rendered
    pixels; the sources' closing
    at a click around it, and shutting with the panel for good), and the arrow between two effects adding one there;
  - picking the oversampling, MPE and its bend range (leaving the patch alone), the theme and the depth handles'
    size (drawn bigger at each, read off the rendered pixels), and dragging the glow slider and the lines slider (a source's dashed line drawn as strongly as it says, and not at all when off, read off the rendered pixels), in the settings dialog (with the editor rendered in each theme, and glowing), closing it with its x
    or a click around it, and the wheel scrolling nothing behind it;
  - undo and redo of drags, wheel runs, node moves, typed values and a removed effect with its modulation;
  - typing values into knobs, header knobs and target depths, with Enter, Escape and a click elsewhere;
  - the preset bar's arrows (asking first while the patch has changed, the dialog a line shorter before a file
    loads); Save greyed out (read off the rendered pixels) and doing nothing while the preset is unchanged, asking
    before it overwrites the preset's file (Cancel keeping it, a failed write leaving the patch changed), and
    doing nothing on Init, changed or not, where Save As does nothing until the patch changes and the preset browser, which loading a preset closes; its search narrowing the list as words are
    typed, Enter loading the preset found, Up and Down picking one, Escape and the search's × emptying it, and
    its editing: selecting with Shift and the arrows, Home and End, the mouse (a drag, a Shift+click, a
    double-click for a word), cutting and pasting;
  - drawing a Custom wave's partials, at 16 and 64 of them too (the wheel stepping through the counts), the ones a lower count hides kept as they were, the
    edit button (a pencil) by the keyboard switch opening the partial editor and closing it (read off the rendered
    pixels), and the full-size LFO shape editor hiding the inspector;
  - a Custom LFO's card: picking Custom widening it, and so a click on its display (the next card moving along), the inspector's knobs turning meanwhile, its shape's points added and dragged in the card, its
    tools inverting, reversing and shifting the shape (each one undo step) and copying it into another LFO (one card
    widened at a time), its title row's expand button widening it as a click on its display does, Steps drawing a step with a plain click and Snap snapping a point unless Ctrl is held, its
    full-size editor greying the card's editor out (taking no clicks) until it closes, by its done button or a view
    tab, the card staying widened, also while the modulation panel collapses, and the
    card back to its size by its collapse button or another shape;
  - an LFO's shape list (going with the LFO undone), the random LFO's knobs and its dot waiting at the end of a cycle (read off the rendered
    pixels), the list of more waves (Organ among them), the shape editor's grid, snapping and reset button, a point dragged
    onto its neighbor's time, and
    scrolling the source cards by holding a dragged handle at the panel's edge;
  - scrolling the effects and the sources by holding a card being moved at either end of its row, where it lands; a
    source card moved within its own place, or carried out of its panel, moving nothing;
  - dragging the effects, the source cards, the MODS view's rows and the preset browser's list with the middle
    button held: over a knob without turning it, stopping at the row's end and following back from there at once,
    and doing nothing while the left button drags a knob;
  - eight modulations on a knob and six on a matrix cell showing as many depth handles as fit and a "+n" pill,
    nothing drawn outside the knob's column or in the next cell (read off the rendered pixels); the pill's list
    bypassing, setting the depth of and removing its modulations and closing at a click elsewhere, opening under a
    macro's handle held over the pill and taking it as a row's aux, widening for an aux column whose chip (read off
    the rendered pixels) and its × take the aux away, and a source's line ending at the pill holding its handle;
  - a depth handle's right-click menu removing its aux, undoably, offered only while it has one;
  - a hovered depth handle drawing its range wider, over the others' (read off the rendered pixels);
  - an aux's handle hovered drawing a dotted line to the depth handle it scales, and that depth handle hovered
    drawing it from the aux's handle (read off the rendered pixels);
  - an LFO's handle showing its "Drag to assign" tooltip only until it modulates something (read off the rendered
    pixels);
  - the title bar's tempo: none for a plugin host, and for a host that lets the editor set it a field (read off the
    rendered pixels) dragged a BPM every 2 px (a tenth with Shift), the wheel stepping it a BPM a notch onto whole
    ones (a tenth with Shift), held to 20-999, and typed after a double-click or Ctrl+Click (`BPM` after it taken,
    text that isn't a tempo and Escape leaving it), none of it touching the patch or its undo history.

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
  - playing: an A4 at 440 Hz, bent through the MIDI-mapped pitch bend, tuned an octave by a note expression on its id
    (and not by one on another id), its level set by a volume expression, automation landing at its place in a
    block, a param change coming before a note at the same place, notes let go (with ids and without) falling
    silent, an anchored LFO standing where the process context's position puts it, more changes than a block sorts
    (a note's end past them still ending it, a param's last point winning), a flush without audio taken, the
    controller's copy of a value from the host left alone while processing, and a voice limit past the voices
    allocated asking the host for a restart;
  - the state: the CLAP plugin's bytes, through a stream that moves 5 bytes at a time, the controller's own state
    empty, a value from the host taken while not processing, loading bringing the saved value back and having the
    host read the values again, and a foreign blob refused;
  - the editor in a hidden host window: one at a time, resized keeping its aspect ratio, the Volume knob dragged
    (beginEdit, performEdit, endEdit, and an older value the host hands back right after not taken), a state with
    the modulation panel collapsed asking the host for a shorter window, its window gone once it is removed, and
    opening again after.

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
  MIDI input it can open, at a tempo of 140 BPM for synced LFOs and effects, its transport playing from its start
  for anchored LFOs. It offers the plugin FMVibe's own host extension for the tempo (`src/hosttempo.h`), so the
  editor's title bar shows the tempo, to change as it plays. With
  `--silent`, or without a device, it still runs the plugin in real time into silence, so the meters, the scope and
  the analyzers move; `--no-midi` leaves the MIDI inputs alone and `--tempo <bpm>` sets another tempo. It restarts
  the plugin when a higher voice limit asks for it, and runs its main-thread callbacks. It isn't a test, and like a
  DAW it is for a person to start: it waits for its window to be closed. `.\build.ps1 -Run` builds it and starts it
  after the other steps.

## Linux development build

Cloud sessions run on Linux, where the plugins can't be built (Win32, Direct2D, the plugin hosts). What can be
built there, `./build.sh` builds: the engine and the editor (`fmvibe-core`, drawing through Cairo in
`src/gui/canvas_cairo.cpp`), the audio quality test, the UI snapshots and the preset tool (`cmake/linux.cmake`, which
`CMakeLists.txt` takes in on any system but Windows). It needs CMake, Ninja, a C++20 compiler (GCC 13 or Clang 18),
pkg-config, git and Cairo's headers (`libcairo2-dev`), and fetches the CLAP SDK with a git clone of the tag
`CMakeLists.txt` pins, since the cloud sessions' proxy refuses GitHub's archive downloads.

```bash
./build.sh                  # build/linux: build, then ctest (quality, ui); about a minute from scratch
./build.sh --skip-tests     # build only
./build.sh --debug          # build/linux-debug
./build.sh --shots 2        # render the snapshots only, at 2x, into build/linux/ui-shots
```

- **Looking at the editor**: the snapshots are PNGs in `build/linux/ui-shots`, one or more for each step that
  `tools/uishot.cpp` replays; an agent can open them as images. To see a change, render the snapshots and open the
  ones its replay saves, or add a replay that saves one.
- **What differs from Windows**: the text is drawn in Inter (`FMVIBE_FONT` picks another family), not Segoe UI, so it
  comes out a little wider, and Cairo's antialiasing isn't Direct2D's. A UI check that reads a pixel should read it
  clear of text and of edges whose place follows a text's width, so it passes with either font.
- **Not covered**: the CLAP and VST3 smoke tests, the validators, the editor's window and `fmvibe-uihost` need
  Windows. A change to `src/plugin.cpp`, `src/vst3plugin.cpp`, `src/plugincore.cpp`, `src/gui/editor.cpp` or
  `src/gui/canvas.cpp` compiles only there; `build.ps1` on Windows (or the release workflow) is the full check.

## Factory presets

The factory presets are preset files, `presets/Factory/<name>.fmv`, which the build compiles in:
`cmake/embed_presets.cmake` turns them into byte arrays (`factoryPresets()` in `src/factory.h`), for the plugin and
the browser demo alike, again whenever one changes. The plugin writes their bytes as they are into a new preset folder.
`presets/factory.txt` names them in the order they came, which counts: the settings file counts the examples a folder
was given, so a new one goes at the end and none is reordered or taken out. The build fails on a file the list
doesn't name, or a name without its file.

They are stored sparse: only the params that differ from their defaults (which is what loading a preset gives the
others), then the editor layout, a kilobyte or so where a preset the plugin saves holds every param (about 128 KB).
To add or change one:

1. Make it in the plugin and save it into `presets/Factory` (the preset bar's Save, then the file dialog).
2. Make the file sparse: `build\release\fmvibe-presettool.exe "presets\Factory\<name>.fmv"`.
3. For a new one, add its name at the end of `presets/factory.txt`.
4. Build and run `ctest`.

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
- Presets are the factory ones, held in memory. Load, Save and the preset folder show a message instead, since the
  demo has no files. The theme, glow, handle size and line strength are kept in the browser's storage.
- The computer keyboard plays notes: A W S E D … from middle C, with Z and X to shift the octave and C and V to step
  the velocity (1, 20, 40 … 100, 120, 127; 100 to begin with, as the editor's keyboard always plays).
- The page stands in for a host's transport: its `Controller` offers the tempo (`hasTempo()`), which the editor's
  title bar then shows to drag, scroll or type (20–999 BPM, kept in the browser's storage), and which `dsp_tempo()`
  hands the synth as `Plugin::process()` does the host's tempo.
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
(`LICENSE-VST3`) into `FMVibe-<tag>-windows-x64-vst3.zip`. It then creates a GitHub release for the tag with both
zips and notes generated from the commits since the previous tag. Meanwhile it builds the
browser demo on Linux, with the Emscripten version `build-web.ps1` pins, and publishes it on GitHub Pages
(`https://pcdlin.github.io/FMVibe/`). Neither goes out unless both build.

`.\release.ps1` does all of it: it needs `main`, a clean working tree, `main` level with origin and a free tag. It
raises the minor version in `project()` at the top of `CMakeLists.txt` (`-Version 1.0` picks another), runs
`build.ps1 -Validate` (every test and both validators), and copies the snapshots the tests render over the README's:
PD Crystal Motion's routing view over `docs/editor.png` and its effects' panel as a chain over `docs/fx-chain.png`, and
BA Neon Pluck's effects' panel, a graph split low and high, over `docs/fx-graph.png`. It then lists what it would do and asks; only an answer of `yes` commits
`CMakeLists.txt` and the three screenshots as "Version X.Y" on `main`, tags it `vX.Y` and pushes `main` and the tag.
Any other answer puts those files back as they were.

By hand, instead:

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
| `src/presets.cpp` | The preset library: the preset folder, its scan, the examples written into it |
| `src/usersettings.cpp` | The user's settings file in the roaming app data (theme, glow, handle size, line strength, examples given) |
| `src/sharedtelemetry.h` | The telemetry the audio thread hands the editor, in atomics |
| `src/params.h` | Parameter layout (index ↔ id), slot targets, value mappings |
| `src/params.cpp` | Parameter table (names, ranges, defaults) and value text |
| `src/uistrings.inc` | The wording of every piece of text the plugin shows, by name; `src/uistrings.h` looks it up |
| `src/synth.cpp` | FM voices, matrix, filter routing, LFOs and mod envelopes, telemetry; independent of the plugin formats |
| `src/wavetable.cpp` | Band-limited operator wavetables, one per octave |
| `src/zerobuffer.h`, `src/zerobuffer.cpp` | Delay lines allocated as zero pages that only take memory once written |
| `src/filter.h` | State-variable and comb filters, plus their response curves for the editor |
| `src/clipcurve.h` | The shapers' curves, anti-aliasing and running, shared by the shaper effect and the voices' shapers |
| `src/amp.h`, `src/amp.cpp` | The Amp effect's sound: its models (preamp stages, tone stack, power amp), cabinets and mics, its oversampling, and the responses the editor draws |
| `src/analyzer.cpp` | The spectrum analyzer (FFT) behind the Param EQ's, filter effect's, phase disperser's and voice filters' displays |
| `src/envelope.h` | AHDSR used by operators and mod envelopes |
| `src/gui/view.cpp` | The editor: its widgets, drawing and mouse and keyboard handling, and the model access behind them. Window-free, so it also renders offscreen |
| `src/gui/theme.h`, `src/gui/theme.cpp` | The editor's color themes, the colors of the one being painted, and the graphs' glow |
| `src/gui/layout.h` | Where the editor's panels, cards, buttons, lists and dialogs go, in logical pixels |
| `src/gui/icons.h`, `src/gui/icons.cpp` | The editor's icons (effects, filter types, band shapes, LFO modes, waves) and the point cache behind them and the graphs' curves |
| `src/gui/routing.h`, `src/gui/routing.cpp` | The routing graph: which param joins two nodes, wires and arrowheads; connecting and rerouting |
| `src/gui/fxgraph.h`, `src/gui/fxgraph.cpp` | The effects' line of effects, splits and joins, read from the wires and written back to them; its two views, the row (each split a container holding its bands) and the graph's slot grid with its +s and inspector; and a split's controls, which both show |
| `src/gui/shapes.cpp` | Editing drawn shapes: freeform LFOs, shaper curves and the note source's curve |
| `src/gui/mods.cpp` | The MODS view: every modulation as a row, its filter, and the cascading menus (the target's with a search) that move it, retarget it and pick its aux |
| `src/gui/dialogs.cpp` | The settings and confirm dialogs, the preset browser with its search, and the text fields' editing |
| `src/gui/canvas.cpp` | Small Direct2D/DirectWrite drawing layer |
| `src/gui/canvas_web.cpp` | The same drawing layer over the browser's 2D context, for the demo |
| `src/gui/canvas_cairo.cpp` | The same drawing layer over Cairo, for the Linux development build |
| `src/gui/editor.cpp` | Win32 child window that hosts the view in the plugin window |
| `src/gui/controller.h` | What the editor needs from the plugin (values, edits, telemetry, layout) |
| `src/gui/controller.cpp` | The undo history, which the plugin keeps for the editor |
| `src/factory.cpp` | The factory presets' loading; the presets themselves are `presets/Factory/*.fmv`, built in by `cmake/embed_presets.cmake` (see [Factory presets](#factory-presets)) |
| `presets/` | The factory presets' files, and `factory.txt`, the order they came in |
| `tools/presettool.cpp` | `fmvibe-presettool`: makes preset files sparse, as the factory ones are stored |
| `tests/smoke_host.cpp` | Headless CLAP test host |
| `tests/vst3_host.cpp` | Headless VST3 test host |
| `tools/uishot.cpp` | Offscreen editor renderer and interaction checks |
| `tools/uihost.cpp` | `fmvibe-uihost`: a small CLAP host with a window and audio, for trying the editor by hand |
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
  room: globals 0+, operators 100 + 50·op (their filter and shaper sends, then Fixed, Freq, Mult and Phase; the
  9th field, once Velocity, does nothing and is hidden, kept so the ids after it stay, as are the first two, once
  Coarse and Fine, since Ratio after Phase), matrix
  400 + 10·from + to, filter modulation
  500 + 10·filter + to, shapers 600 + 50·shaper, filters 800 + 20·filter, filter links 900+, effect slots
  1400 + 80·slot (their fields past
  the 80th 12000 + 20·slot, past the 100th 25000 + 40·slot, and past the 140th 36000 + 20·slot; the slots past the
  first 16 every field
  26000 + 200·(slot - 16)), source places
  2800 + 40·place, freeform shapes 4000 + 100·place (their points past the 33rd 15000 + 100·place), the sources'
  later slots 6000 + 30·place, shaper effects' curves 7000 + 50·slot (their points past the 16th
  17000 + 200·slot; the slots past the first 16 every field 30000 + 200·(slot - 16)), the chain's order 33400 + place,
  slot bypasses 9000 + 20·place (then the macros', the note source's, the mod wheel's and
  velocity's, 9320 + 20 a source by the sources' number), Custom waves' first 32 partials 10000 + 100·op, macros
  11000 + 40·macro, the note source 11400+ (its slots, then its range and curve), the mod wheel's slots 11500+,
  velocity 11600+ (its slots, then its curve and Bipolar), the
  Custom waves' partial counts 14000+ and their partials past the 32nd 14100 + 100·op, the routes' modes
  700 + 10·from + to (from by the routing graph's node order: the operators, the filters, the shapers; to an
  operator; an operator's own field, its feedback, hidden and always Phase), the voice shapers'
  curves 13000 + 50·shaper (their points past the 16th 21000 + 200·shaper), every slot's aux and Amount
  22000 + 40·source (a pair per slot, by the sources' number: the places, the macros, the note source, the mod wheel
  and velocity; a source added later takes the next block), and every slot's curve 24000 + 20·source (by the
  sources' number likewise); the effects graph's splits 34000 + 20·split, the wires into its
  slots and splits 34200 + 20·node (the slots, then the splits; a From and a Level an input) and the wires into its
  output 35000+, its sums 35100 + 20·sum and the wires into them 35200 + 20·sum. A wire's From stores its port + 1
  (0 = none): the input, then each slot's output, each split's bands, low to high, then each sum's output. The graph's
  nodes are numbered the slots, the splits, the output, then the sums. The editor wires a line (see
  `src/gui/fxgraph.cpp`): a split's two bands are its low and high ones (Bands 2), and its join is the sum of its
  number, whose first two inputs take the high band and the low band. The effects always run as the graph: a chain
  is a line of it (the output's first wire comes from the input to begin with), and the FX view's row and graph are
  two views of it, the view a layout field (`UiLayout::fxGraphView`). The chain's places (33400 + place) are only
  read when loading a state from before version 9.
  Once released, new fields go at the end of their section's
  enum and fields are never reordered. The same goes for the wave, filter type, effect type, source type, route
  mode, slot aux field, split field, wire field, port, amp model, cabinet, mic and fixed Mult lists, the routing views and the operator page's tabs,
  which are stored by number. `src/params.h` pins each section's first and last ids, the counts they come from and
  the stored lists' members with static_asserts, so a change that renumbers them doesn't compile; a field added to
  `FxParam` also needs its effect type in `kFxFieldRuns`. A retired type keeps its number and fields: the
  three-band compressor (`FxType::MultiComp`) is no longer offered, and `readParams()` loads a slot holding one in a
  state from before version 6 as a three-band dynamics set from its fields, moving the modulation aimed at them
  along. A host can still set the retired type (the Type param's range takes it in): such a slot does nothing, and a
  state keeps it as it is.
- Like an effect slot, each of the 16 source places has the fields of every type (an LFO's, an envelope's, and
  Slew, CC, Bipolar and Input for Random and MIDI), and a Type param (Empty, LFO, Envelope, Random or MIDI) that
  picks which are used (`sourceFieldUsedBy()`). Hosts are given only a place's slot depths and Amounts; its fields go by place
  ("Mod 3 Rate"), since a name can't follow the type. A MIDI source takes CLAP MIDI events
  (controllers, channel and polyphonic aftertouch) and CLAP's pressure and brightness note expressions, as its Input
  picks.
- Hosts are given 1,226 of the 19,153 params (`hostParamCount()`, `hostParamIndex()`, `hostListed()` in
  `src/params.h`), 864 of them the slots' depths and Amounts. The editor's alone are those the table marks
  `CLAP_PARAM_IS_HIDDEN` (the drawn shapes and curves, the Custom waves' partials and counts, the chain's retired order, the
  feedback's route modes and the retired fields), every slot's target, aux, curve and bypass, the effect slots'
  fields, the effects graph (its splits, joins and wires) and the source places' own fields (a place's slot
  depths are listed, whatever it holds). Nothing hosts
  see changes with an effect's or a source's type: the CLAP and VST3 plugins hide none of their params. The remote
  pages follow the sources and the operators switched fixed. CLAP's `count()`/`get_info()` and VST3's
  `getParameterCount()`/`getParameterInfo()` index that list; a value sent for any known id is still taken. The
  state, presets, undo and the editor's modulation cover every param. The core tells hosts of edits to listed ones
  only, and marks the project dirty for the others.
- A slot's *target* param stores the target's id + 1 (0 = unused). Each slot also has a depth, an aux (the
  source + 1 that scales it, 0 = none) and an Amount (0–1, 1 to begin with), whose defaults leave a slot as it was.
  Depths and Amounts are automatable params; targets and auxes are wiring, set in the editor and not given to
  hosts, but saved like any other param. A slot swings its target one way or both as its source's
  own switch says.
- Edits in the editor reach the host as `PARAM_GESTURE_BEGIN` / `PARAM_VALUE` / `PARAM_GESTURE_END` events (in
  VST3, `beginEdit()` / `performEdit()` / `endEdit()` on the main thread). Hosts
  record automation and undo from these, and mark the project as changed. Loading a preset, or Init, sends them for
  each param whose value changed. The queue they wait in keeps room for the end of each gesture begun, so a host is
  never left inside one (a gesture whose begin didn't fit goes without its end); edits that don't fit have the host
  read every value again instead (one values rescan, from the main-thread callback). For half a second after the plugin
  tells the host a param's value, it takes no value for that param from the host: Bitwig hands values back a
  little later, and taking them would flip a preset loaded meanwhile back to the one before, for a moment. VST3 hosts
  hand every edit back to the processor, which the same moment keeps out.
- The state blob holds `(id, value)` pairs with a version number (currently 10: version 1's Volume loads 12 dB
  lower, since Volume now gives back the 12 dB the voices go into the effects with; before version 3, a state in
  poly loads Legato off, since it did nothing there then and acts there now; before version 4, a modulation's depth on an
  envelope's attack, decay or release loads scaled by `kMaxEnvNorm / (kMaxEnvNorm - kMinEnvNorm)`, since their
  range now reaches below 0 down to 0 ms and a depth is a share of its target's range; before version 5, an
  operator's ratio loads from its Coarse and Fine, and their modulations move onto Ratio; before version 6, a slot
  holding the retired three-band compressor loads as a three-band dynamics, while a later state loads one back as
  saved, so a state always loads what was saved; before version 7, a modulation's depth on Glide loads scaled by
  `1 / kMaxGlideNorm`, since its range now stops at 6 s; before version 8, the chain's order loads from the retired
  `RetiredFxOrder`, the rank of the first 16 slots' order, onto its places, a param a place, since 32! wouldn't fit a
  double; before version 9, a state that doesn't wire the effects graph's output, saved with the effects as a chain,
  loads its chain's order as the graph's line (`fxChainWires()`), since the wires alone put the effects in order now,
  and the places do nothing; before version 10, a modulation's depth on a three-band dynamics' Above or Below threshold
  loads scaled by `kMdRetiredThresholdMinDb / kMdThresholdMinDb`, since their range now reaches down to -80 dB rather
  than -60), followed by the editor layout:
  node, filter and shaper positions, selected node, zoom, the routing view, whether the modulation panel is collapsed,
  the order the sources show in, the routes kept at 0 %, the shape editor's grid, the macros' names, which
  effect and source cards are folded, whether the keyboard strip is collapsed, the operator page's tab, the
  scope's cycle count, the wires bent away from their first bow (by route), the MODS view's empty modulations
  (each its source + 1 and its target's id + 1, 0 for none), how shapes are edited (Steps, then Snap), which
  effect cards past the 16th slot are folded, the node the effects graph's inspector shows, whether the FX view shows
  the graph, the band each split shows and which splits' containers are folded.
  The host's state goes on with
  the settings chunk (the oversampling, MPE and MPE's bend range) and a preset chunk (whether the patch changed since its preset, then the preset file's path
  in UTF-8, left out past 64 KB); preset files stop after the layout. Params and layout fields a
  state doesn't have keep their defaults; a layout or settings chunk longer than the loader knows (up to 4096
  floats are read) keeps the fields it knows and is skipped past, so the chunks after it still load; a chunk cut
  short keeps the floats that arrived whole, and a layout value that isn't a number keeps its default. States
  with a newer version are refused.
- Times and LFO rates are stored as 0–1 on a log curve (1 ms–20 s, 0.02–100 Hz), so host knobs and modulation
  have resolution where it matters. The value text shows ms/s/Hz. Envelopes' attacks, holds, decays and releases
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
  note that is sounding.
- Voices are allocated when the host activates the plugin, for the current Voices value; the audio thread never
  allocates. Lowering the limit takes effect at once: the voices past it fade out as stolen ones do. Raising it past what is allocated calls
  `host->request_restart()` (in VST3, `restartComponent(kLatencyChanged)`, on the main thread), and the host then
  deactivates and reactivates the plugin (a host that doesn't is asked again once more voices or another
  oversampling factor need one). What VST3 only allows on the main thread, and the audio thread may ask for,
  goes through a message-only window there. Each voice takes about
  66 KB at 48 kHz, mostly the two comb filters' delay lines, which grow with the oversampling: at the default
  2x, about 105 KB, so 512 voices reserve about 54 MB. Voices isn't automatable for the same reason. The comb lines,
  and the effects' delay lines (every slot has every type's, a few MB a slot), are `ZeroBuffer`s
  (`src/zerobuffer.h`): pages Windows hands out as zeros once they are first written, so the lines no voice or
  effect uses take address space, not memory, and allocating them fills nothing.
- The CRT is linked statically. The `.clap` and the `.vst3`'s DLL depend only on Windows system DLLs (kernel32,
  user32, Direct2D, DirectWrite, and ole32 for the VST3 one), not on the VC++ redistributable. The `.vst3` is a
  bundle: `FMVibe.vst3\Contents\x86_64-win\FMVibe.vst3`.

## Updating the CLAP and VST3 SDKs

Change the tag in the `FetchContent_Declare(clap ...)` URL in `CMakeLists.txt` and replace the hash with the new
archive's SHA-256 (`Get-FileHash <file> -Algorithm SHA256`). The VST3 SDK comes as three archives of the same tag:
`vst3_pluginterfaces`, which the plugin is built against, and `vst3_base` and `vst3_public_sdk`, which only the
validator is built from; update the three together. Its interfaces' version also goes into the class info hosts
see.

## Next steps

- Presets: the `clap.preset-load` extension and preset discovery, so hosts can browse FMVibe's presets too.
- Sound: fixed-frequency operators, key scaling, oversampling for high-index patches.
- AU and standalone builds: [clap-wrapper](https://github.com/free-audio/clap-wrapper) builds these from the
  CLAP plugin. The plugins' editor is Windows-only for now (the browser demo has its own drawing backend).
- VST3: a `moduleinfo.json` in the bundle, for hosts that scan with it.
