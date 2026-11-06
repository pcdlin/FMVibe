# AGENTS.md

Notes for anyone (human or agent) working on FMVibe, a six-operator FM synthesizer as a CLAP and a VST3 instrument
plugin for Windows. The README introduces the plugin and says how to build and install it. What the plugin does and how
its editor works, in full, is in [docs/features.md](docs/features.md): read the parts you need before changing a
feature.

## Ground rules

- Parameter ids, the wave, filter type, effect type, source type and fixed Mult lists, and the state layout are
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
- The editor draws through `Canvas`, which has two backends: Direct2D (`src/gui/canvas.cpp`) and the browser
  demo's 2D context (`src/gui/canvas_web.cpp` with `web/site/canvas2d.js`). A new drawing call needs both.
- Text the plugin shows (labels, tooltips, menus, messages, parameter names, remote control pages) is worded in
  `src/uistrings.inc` and looked up by name: `tr(Str::Name)`, or `trf()` for a wording with `{1}`, `{2}` in it. New
  text gets a line there. Tests that match on text (menu entries, page names) look it up the same way, so a
  rewording keeps them green.

## Build

What you need, `build.ps1` (which runs the tests below with every build, unless `-SkipTests` builds only the plugins),
the plain CMake commands and installing into a host are in the [README](README.md#build).

## Testing

`ctest` runs four programs:

- **Smoke test** (`tests/smoke_host.cpp`): a small headless host. It loads the `.clap` and checks the parameter
  table, text round-trips (at each end and just inside it too, the slowest sweep rate and the shortest time typed
  neither still nor Off), the note source's range (every MIDI note, on to the pitch at 20 kHz) and remote-control pages. It then plays
  notes, checking:
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
    voice-info after reactivation, and a state with a higher one, loaded while active, requests one too.

  It opens the editor inside a hidden host window and drags the Volume knob with mouse messages. It checks that
  the param changes and that the host receives gesture-begin, value and gesture-end events, and that an older
  value the host hands back right after isn't taken. It saves/loads state
  through a stream that only moves 5 bytes per call, a version 1 state's Volume loading 12 dB lower, a version 2
  state's Legato loading off in poly (and as it was in mono), envelope attacks, decays and releases typed down
  to 0 ms through fractions of a millisecond, a stored 0 loading as 1 ms and a version 3 state's depths on them
  loading scaled by as much as their range grew, a version 4 state's ratio loading from its Coarse and
  Fine, their modulations moved onto Ratio (a version 5 state's Coarse and Fine doing nothing), envelope times
  stored past 10 s loading as 10 s, a version 6 state's Glide of 20 s loading as 6 s and its depth on Glide scaled
  by as much as the range shrank, a layout as large as one gets among them (and one of NaNs,
  which loads as the defaults), and checks that
  every remote control page has an id of its own. It also reports CPU cost: 16 voices × 6 operators take about a third of what they took
  before the engine's per-block route lists and shared params, which on the development machine was about 22 % of
  one core at the default 2x oversampling and about 38 % with every operator at 8 unison copies (now roughly 7–8 %
  and 25 %; the smoke test prints the figures).
- **Audio quality** (`tests/quality.cpp`): drives the engine directly and measures, at every oversampling factor:
  - the decimator's passband ripple and alias rejection;
  - aliasing (the energy off the harmonics, 20 Hz–20 kHz) of a high saw, bright FM (phase, linear and ring),
    feedback and a driven filter;
  - zipper noise from an LFO on an operator's level, a matrix amount and a filter cutoff;
  - clicks when a voice is stolen, when a note joins a held one in poly legato (fading in over 5 ms, its Random
    LFOs carrying on with the held one's step), and when a param (Volume, and an effect's: a compressor's Makeup, a delay's and a
    reverb's Mix, a DJ EQ band, a three-band dynamics' Output) jumps while events split the block into single frames;
  - the three-band dynamics on a sine in its mid band: a quiet one lifted and a loud one pulled down as its
    thresholds and ratios say (the band telling the editor its level and gain), the lift capped by Range, peaks
    followed with RMS off, 1:1 both ways leaving the level alone, ∞:1 holding a loud one at Above (its text
    reading back), and linked bands still playing their own settings (Link being the editor's); and a
    version 5 state's retired three-band compressor loading as a three-band dynamics doing what it did, its
    modulation along (a linked one's bands taking the low band's settings), while a version 6 state loads one back
    as saved;
  - the reverb's tails: every algorithm's late spectrum without lasting resonances and its two sides unalike, an
    undamped tail falling 60 dB around 1 kHz in its decay time, and a reverb with a 200 ms pre-delay ringing on
    until its tail has been heard, and stopping after;
  - a drawn shaper curve's points moved gliding in rather than clicking, in whole blocks and in single frames, and
    dragged up and down putting out no DC, made symmetric too;
  - a quiet, slow sine through a drawn shaper curve coming out as drawn, without spikes where it crosses the
    curve's table steps; a symmetric drawn curve that doesn't pass through the middle putting out silence for
    silence, and an input that isn't a number reading inside its table;
  - the filter effect switched to another type or slope and back, and a plate switched to a hall and back,
    playing nothing their stages held before; and the effects running on until a shaper's DC blocker has settled;
  - the random LFO's steps and glides, a freeform LFO's points at one time (a vertical line, its last
    counting, and a point at the cycle's end holding a one-shot there), the phaser's and flanger's sweeps synced to the host tempo, the parametric
    EQ's, the phase disperser's and the voice filters' analyzers, and that each example preset holds valid values
    and sounds at a sane level;
  - each factory preset's file being sparse, as the preset tool writes it;
  - the mod wheel leaving the FM alone until it is assigned, and the output meter's peaks, fall and clip light;
  - Volume setting the level after the effects, and a mod envelope's level half way through its release;
  - every note reporting its end once: in mono a note taken over in its release, with note ids a note on a key
    struck again while it rings, the voices a lower voice limit cuts, a note with an id while its key sounds on
    without one, mono's all notes off (releasing on the key it plays), a reset and forty notes at once on one voice;
    a chord into voices that have all played taking the oldest, every note of it sounding (and a slow attack's new
    note not stolen first); a voice filter whose send goes to 0 ringing out; a velocity or note expression that isn't
    a number staying finite; host modulation of a macro, the mod wheel, the note source's depth and velocity's
    moving their targets; a comb never reading past its line; a voice's right channel coming back from mono where
    the left is;
    an envelope's curve bent again after a straight stretch carrying on along it; and a 0 ms attack, decay and
    release taking a sample each, a 0.5 ms and a 1 ms attack their time, a sustain change after a 0 ms decay still
    smoothed, and a voice with a 0 ms attack at full level from its first sample;
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
  - assigning LFOs and envelopes to a knob, a header knob and a wire label, and turning a knob;
  - connecting operators by dragging from a node's edge (lit under the mouse), anywhere around it, while a click
    there only picks the node and a drag from further inside moves it; a filter's edge dragged onto an earlier
    filter feeding it, and a filter's and a shaper's dragged back onto itself feeding it back; a filter going out by its edge dragged onto
    the bus, with nothing going in, and stopping by its arrowhead dragged away; routing a carrier through a filter,
    which goes out in its place, and picking waves and filter types;
  - moving routes by dragging their arrowheads onto another node and onto the OUT bus, with their modulation, and
    removing one by letting its arrowhead go in empty space; a drag called off as the mouse is taken away dropping
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
  - the MODS view's filter: its magnifier opening it to take the keyboard, the words typed (every one, in any case)
    narrowing the rows, its × emptying it, Escape and a click elsewhere letting the keyboard go; and the target
    menu's search listing the targets found with their path, the first that can be picked lit (read off the rendered
    pixels), Up and Down picking one, Enter taking it, Escape emptying it and then closing the menu; an operator's
    targets in two columns, the envelope's beside the pitch's;
  - the modulation panel's title row adding Random and MIDI sources, each with its card, a MIDI card's knob picking
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
  - an operator lighting up while modulation opens its route from 0 % (read off the rendered pixels);
  - the node the inspector shows filled and ringed in the graph, picking another moving both, and a switched-off
    one's ghost filled while it is shown (read off the rendered pixels);
  - a switched-off source's handle going faint, and a macro's and the note source's until they modulate something
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
  - dragging a Param EQ band's handle, and Alt-dragging it for its Q;
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
    off the rendered pixels), and LINK (an edit of one band, by its line or knob, then setting every band's, undoably) and RMS;
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
    loads) and the preset browser, which loading a preset closes; its search narrowing the list as words are
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
  - scrolling the effects and the sources by holding a card being moved at either end of its row, where it lands;
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
    pixels).

  Pass a scale to render larger: `build\release\fmvibe-uishot.exe out 2`.
- **VST3 smoke test** (`tests/vst3_host.cpp`): a small headless VST3 host. It loads `FMVibe.vst3` and checks what the
  VST3 side adds to the engine the tests above check:
  - the module's exports and its factory: one class, an Instrument|Synth at the project's version, which a host can't
    ask for an interface it doesn't have; the component being its own controller, with MIDI mapping, note
    expressions, units and its process context needs (the tempo, and the transport's state and position);
  - the buses (a stereo output, refusing mono, and a 16-channel note input) and 32-bit processing only;
  - the params: every one, with the CLAP plugin's ids, all different, each in a unit the plugin lists, going to
    normalized and back unchanged (a stepped one exactly), their defaults, a value's text and back, Voices stepped
    and not automatable, the MIDI controllers' params hidden, and an effect's fields showing once its slot holds it,
    with the host told; the MIDI mapping (per channel, on the note input only), the note expressions and the
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
  MIDI input it can open, at a tempo of 120 BPM for synced LFOs and effects, its transport playing from its start
  for anchored LFOs. With
  `--silent`, or without a device, it still runs the plugin in real time into silence, so the meters, the scope and
  the analyzers move; `--no-midi` leaves the MIDI inputs alone and `--tempo <bpm>` sets another tempo. It restarts
  the plugin when a higher voice limit asks for it, and runs its main-thread callbacks. It isn't a test, and like a
  DAW it is for a person to start: it waits for its window to be closed. `.\build.ps1 -Run` builds it and starts it
  after the other steps.

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
- The page stands in for a host's transport with a Tempo field under the editor (20–999 BPM, kept in the
  browser's storage), which `dsp_tempo()` hands the synth as `Plugin::process()` does the host's tempo.
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
2026) with `build.ps1 -Validate`, so every test and both validators have to pass. It puts `FMVibe.clap`, the
`FMVibe.vst3` folder, `LICENSE` and the CLAP and VST3 SDKs' licenses (`LICENSE-CLAP`, `LICENSE-VST3`) into
`FMVibe-<tag>-windows-x64.zip`, and creates a GitHub release
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

Install the debug build (`.\build.ps1 -Config debug -Install`), which installs both plugins. Bitwig runs plugins in
a separate process, so in Visual Studio use *Debug > Attach to Process* and pick `BitwigPluginHost-X64-AVX2.exe`;
breakpoints in `src/` then hit.

## Layout

| Path | What |
| --- | --- |
| `src/plugincore.cpp` | What both plugins are: `PluginCore`, the engine and its param values, the editor's `Controller` (presets, settings), the state and the editor window, telling each host through its hooks |
| `src/entry.cpp` | The CLAP plugin's exported `clap_entry` and factory |
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
| `src/factory.cpp` | The factory presets' loading; the presets themselves are `presets/Factory/*.fmv`, built in by `cmake/embed_presets.cmake` (see [Factory presets](#factory-presets)) |
| `presets/` | The factory presets' files, and `factory.txt`, the order they came in |
| `tools/presettool.cpp` | `fmvibe-presettool`: makes preset files sparse, as the factory ones are stored |
| `tests/smoke_host.cpp` | Headless CLAP test host |
| `tests/vst3_host.cpp` | Headless VST3 test host |
| `tools/uishot.cpp` | Offscreen editor renderer and interaction checks |
| `tools/uihost.cpp` | `fmvibe-uihost`: a small CLAP host with a window and audio, for trying the editor by hand |
| `build.ps1` | One-shot configure/build/test/validate/install |
| `web/dsp.cpp`, `web/ui.cpp` | The browser demo's audio and editor modules |
| `web/site/` | The demo page: the canvas and input (`main.js`), drawing (`canvas2d.js`), the AudioWorklet (`worklet.js`) |
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
  as VST3 has them; the chain order has more orders than a step count holds, and passes as continuous). After them
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
  the 80th 12000 + 20·slot, and past the 100th 25000 + 40·slot), source places
  2800 + 40·place, freeform shapes 4000 + 100·place (their points past the 33rd 15000 + 100·place), the sources'
  later slots 6000 + 30·place, shaper effects' curves 7000 + 50·slot (their points past the 16th
  17000 + 200·slot), slot bypasses 9000 + 20·place (then the macros', the note source's, the mod wheel's and
  velocity's, 9320 + 20 a source by the sources' number), Custom waves' first 32 partials 10000 + 100·op, macros
  11000 + 40·macro, the note source 11400+ (its slots, then its range and curve), the mod wheel's slots 11500+,
  velocity 11600+ (its slots, then its curve and Bipolar), the
  Custom waves' partial counts 14000+ and their partials past the 32nd 14100 + 100·op, the routes' modes
  700 + 10·from + to (from by the routing graph's node order: the operators, the filters, the shapers; to an
  operator; an operator's own field, its feedback, hidden and always Phase), the voice shapers'
  curves 13000 + 50·shaper (their points past the 16th 21000 + 200·shaper), every slot's aux and Amount
  22000 + 40·source (a pair per slot, by the sources' number: the places, the macros, the note source, the mod wheel
  and velocity; a source added later takes the next block), and every slot's curve 24000 + 20·source (by the
  sources' number likewise).
  Once released, new fields go at the end of their section's
  enum and fields are never reordered. The same goes for the wave, filter type, effect type, source type, route
  mode, slot aux field and fixed Mult lists, the routing views and the operator page's tabs,
  which are stored by number. `src/params.h` pins each section's first and last ids, the counts they come from and
  the stored lists' members with static_asserts, so a change that renumbers them doesn't compile; a field added to
  `FxParam` also needs its effect type in `kFxFieldRuns`. A retired type keeps its number and fields: the
  three-band compressor (`FxType::MultiComp`) is no longer offered, and `readParams()` loads a slot holding one in a
  state from before version 6 as a three-band dynamics set from its fields, moving the modulation aimed at them
  along. A host can still set the retired type (the Type param's range takes it in): such a slot does nothing, and a
  state keeps it as it is.
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
- Edits in the editor reach the host as `PARAM_GESTURE_BEGIN` / `PARAM_VALUE` / `PARAM_GESTURE_END` events (in
  VST3, `beginEdit()` / `performEdit()` / `endEdit()` on the main thread). Hosts
  record automation and undo from these, and mark the project as changed. Loading a preset, or Init, sends them for
  each param whose value changed. The queue they wait in keeps room for the end of each gesture begun, so a host is
  never left inside one (a gesture whose begin didn't fit goes without its end); edits that don't fit have the host
  read every value again instead (one values rescan, from the main-thread callback). For half a second after the plugin
  tells the host a param's value, it takes no value for that param from the host: Bitwig hands values back a
  little later, and taking them would flip a preset loaded meanwhile back to the one before, for a moment. VST3 hosts
  hand every edit back to the processor, which the same moment keeps out.
- The state blob holds `(id, value)` pairs with a version number (currently 7: version 1's Volume loads 12 dB
  lower, since Volume now gives back the 12 dB the voices go into the effects with; before version 3, a state in
  poly loads Legato off, since it did nothing there then and acts there now; before version 4, a modulation's depth on an
  envelope's attack, decay or release loads scaled by `kMaxEnvNorm / (kMaxEnvNorm - kMinEnvNorm)`, since their
  range now reaches below 0 down to 0 ms and a depth is a share of its target's range; before version 5, an
  operator's ratio loads from its Coarse and Fine, and their modulations move onto Ratio; before version 6, a slot
  holding the retired three-band compressor loads as a three-band dynamics, while a later state loads one back as
  saved, so a state always loads what was saved; before version 7, a modulation's depth on Glide loads scaled by
  `1 / kMaxGlideNorm`, since its range now stops at 6 s), followed by the editor layout:
  node, filter and shaper positions, selected node, zoom, the routing view, whether the modulation panel is collapsed,
  the order the sources show in, the routes kept at 0 %, the shape editor's grid, the macros' names, which
  effect and source cards are folded, whether the keyboard strip is collapsed, the operator page's tab, the
  scope's cycle count, the wires bent away from their first bow (by route), the MODS view's empty modulations
  (each its source + 1 and its target's id + 1, 0 for none) and how shapes are edited (Steps, then Snap).
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
  deactivates and reactivates the plugin. What VST3 only allows on the main thread, and the audio thread may ask for,
  goes through a message-only window there. Each voice takes about
  66 KB at 48 kHz, mostly the two comb filters' delay lines, which grow with the oversampling: at the default
  2x, about 105 KB, so 512 voices use about 54 MB. Voices isn't automatable for the same reason.
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
