# FMVibe features

What the plugin does and how its editor works, in full. Building and installing it are covered in the
[README](../README.md), working on the code in [AGENTS.md](../AGENTS.md).

## Sound engine

- **Operators A–F**, each of which can be switched on or off, and each with:
  - a **wave**: Sine, Triangle, Saw, Square or Noise, or one of the less common Pulse (25 %), Half Sine and Abs
    Sine (an octave up, like the classic FM chips), or **Custom**: a wave drawn from its first 32 partials, each
    with a level and a phase (picked after another wave, the partials start as that wave's; otherwise a plain
    sine), band-limited like the others and normalized to its
    peak; the synth builds its tables again whenever the partials change. Noise has no pitch or unison: instead a **Slope** moves it from white through pink (−3 dB per
    octave) to brown (−6 dB per octave) at about the same loudness, and **Spread** (the Uni Spread param) widens it
    from mono to fully uncorrelated left and right;
  - Coarse ratio (0.5, 1…32), **Pitch** (±48 semitones on top of the ratio; the knob moves in whole semitones,
    while LFOs, envelopes and host modulation glide through it), Fine (adds up to +100 % to the
    ratio), Detune (±50 cents) and an AHDSR envelope (by default an organ-like gate: no hold, full sustain and a
    15 ms release);
  - **Hold**: every envelope (operators and mod envelopes alike) can stay at full level for a while after its
    attack, before the decay starts (off by default);
  - **Envelope curves**: the attack, decay and release of every envelope (operators and mod envelopes alike) can
    each be bent from −100 % (starts slow, rushes at the end) through 0 (a straight line) to +100 % (starts fast and
    settles). Every segment arrives in its set time, whatever its curve. By default the attack is straight and
    decay and release sit at +50 %, an exponential fall of 60 dB over the segment's time.
  - Out (level straight to the audio output) and sends into the two filters and the two shapers;
  - Velocity (how much note velocity scales the operator; on a modulator that means brightness);
  - **Unison**: up to 16 copies of the operator. *Uni Detune* spreads them in pitch (the outermost copies by up to
    ±100 cents) and *Uni Spread* across the stereo field. On a modulator, the copies' sum is what its targets
    hear; on a carrier, every copy is modulated by the same signal.
  - **Rnd Phase** (phase randomization): at 0 % (the default) every note starts the waveform, and each unison copy, at the same
    point, so notes sound alike from the first sample; turned up, each copy starts up to that much of a cycle
    later, at random. Unison copies at 0 % start in step and drift apart as they detune.

  Waves come from band-limited wavetables with one table per octave, so saws and squares stay clean high up the
  keyboard, even when they are modulators.
- **Matrix**: `X → Y` sets how strongly operator X modulates operator Y, and `X Feedback` is X modulating itself.
  Any operator can feed any other, cycles included. In each sample, earlier operators (A before B) pass on their
  current output and later ones their previous output; feedback is smoothed over two samples. Amounts are squared
  internally, so the low end is fine-grained. **FM Amount** scales the whole matrix.
- **Filters F1 and F2**, per voice, each switchable on and off (a new instance starts with both off). Any
  operator or shaper can be sent into either filter, and filter 1 can feed filter 2 (`F1 → F2`), for a series
  chain; each filter has its own output level. A filter's
  output can also modulate operators, the same way an operator does (`F1 → A` and so on, scaled by FM Amount):
  noise through a band-pass, for example, gives a carrier a narrow band of grit. Operators hear the filter's
  previous output sample. A filter that only modulates can have its output level at 0. Types:
  - Low-pass, High-pass, Band-pass and Notch: a state-variable filter that stays smooth under fast modulation.
    **Slope** picks 6, 12 (the default), 18 or 24 dB per octave. At 6 dB the filter is a one-pole, taken from the
    state-variable filter so resonance still adds a peak at the cutoff (as high as at 12 dB) while the slope past it
    stays 6 dB per octave; 18 and 24 add a second, plain stage after the resonant one (a one-pole, or a
    Butterworth), so the resonance stays a single peak. A band-pass or notch comes in 12 and 24 dB only: set to 6 or
    18, it plays the slope above;
  - Comb: a tuned feedback delay, so the cutoff sets the pitch and resonance the feedback. With its cutoff at 0
    and the note source on it at 100 %, its curve drawn along the cutoff knobs' (and Key Track left at 0), it
    rings at the pitch of the note.

  Each filter also has Resonance, Key Track (how far the cutoff follows the note from C4 on, an octave per octave
  at 100 %; 0 to begin with, on every filter) and Drive (saturation before the filter, anti-aliased like the
  shapers). A voice only ends once its filters have rung out.
- **Shapers S1 and S2**, per voice, each switchable on and off (off to begin with): the shaper effect in every
  voice, with its curves (Hard, Soft, Asymmetric, Cubic, Gentle, Sine Fold, Fold, or a Custom curve drawn like the
  effect's, the S it starts as included), its Drive (0–36 dB), Output, Mix and Link (on to begin with), and run by
  the same code: anti-aliased the same way, with the same DC blocker after the uneven curves. Any operator can be sent into a
  shaper (`A → S1`), and a shaper's output feeds the filters (`S1 → F1`), for distortion into a filter, and
  modulates operators the way an operator does (`S1 → A`, scaled by FM Amount), so a modulator can be folded or
  squared off before it reaches its carrier. Its **To Out** sends it straight to the output, like an operator's
  Out (0 to begin with, so a new shaper is heard only through a filter or what it modulates). Operators hear its
  previous output sample; like the filters, it runs only while something goes in and its output goes somewhere.
- **Modulation**: up to 16 sources, any mix of LFOs (sine, triangle, saw, square, random or a freeform shape;
  0.02–100 Hz) and mod envelopes (AHDSR, starting at full sustain and with an operator's 15 ms release). A new
  instance has none; each one added starts
  switched on, and a source that is switched off stands still and modulates nothing, but keeps its targets. Each
  has 16 slots, and a slot can
  target any continuous parameter: a knob, a matrix amount, an operator output, even another LFO's rate or a slot
  depth. Depth is ±100 % of the target's range: how far a **unipolar** source takes the target one way, and the
  whole swing of a **bipolar** one, half of it below the set value and half above (at 100 %, −50 % to +50 %).
  Every source can be either, picked from its right-click menu (see the editor): LFOs start bipolar, and
  envelopes, macros, the note source and the mod wheel unipolar. A bipolar envelope swings about its middle level
  (nothing at half of it, half the depth down at rest and up at its peak), and a bipolar macro, mod wheel or note
  source about the middle of what it puts out (a macro moves nothing with its knob at 50 %). LFOs and envelopes run
  per voice and restart
  with each note. LFOs and envelopes see each other's slot modulation one 32-sample block later. Once a key is
  let go, a mod envelope that modulates something runs its release out even after the operators have faded: the
  voice keeps running its modulation alone (rendering no sound) until the release is over, so what it moves, an
  effect's knob for one, follows it down instead of jumping back, and only then reports its note's end to the host.
  - **Macros**: eight knobs (0–100 %) that hosts can automate, each with 16 slots like a source's. A macro's
    value times each slot's depth is added to its target for every voice and the effects alike, so a macro can
    also move effect params, which per-voice sources can't. Their names are part of the patch.
  - **Mod Wheel**: a macro of its own, its knob the Mod Wheel param (the wheel in the keyboard strip), with 16
    slots like a macro's. It moves nothing until something is assigned to it.
  - **Note**: a source that follows the pitch each voice plays, pitch bend, glide (and legato slides), note
    tuning and the global Pitch included. Like a macro it has 16 slots and no switch or bypasses, but it
    runs per voice, like the LFOs and envelopes, and reaches the effects through the newest voice. What it
    modulates follows the pitch within the same block, without lagging behind it.
    Its **range** and **curve** shape what it puts out: the notes from **Low** to **High** (any MIDI note, C-1
    to G9, with their cents; High may lie below Low, which turns it round) run across a curve drawn like a
    shaper's, which sets what each puts out, from nothing to all of it; notes past either end put out what that
    end does. As it starts, the range spans every MIDI note and the curve is a straight line, from nothing at C-1
    to all of it at G9. Over A3 to A5 with a straight line, it would put out nothing at A3, half at A4 and all at
    A5, and a curve drawn the other way round reverses that. The range and curve are part of the patch.
    Drawn along each note's place on the cutoff knobs' curve, 20 Hz to 20 kHz (nothing up to the pitch at 20 Hz,
    D#0 +49, where the knobs start, then a straight line up to where G9, 12.5 kHz, sits on them: the dashed line
    in its editor), it tracks exactly: a cutoff set to 0 and modulated 100 % by it sits right on the note's root
    frequency, and an A4 bent up two semitones takes it to 494 Hz. The filters' Key Track adds its own tracking on
    top, so leave it at 0, where it starts, for an exact match.
  - **Free** makes an LFO run on without restarting at each note. All voices then follow one shared clock, which
    runs at the rate the newest voice hears (like the effects) and keeps going between notes. Off (the default),
    every note retriggers the LFO from the start of its cycle.
  - **Sync** locks an LFO to the host tempo: its rate becomes a note value from 4/1 down to 1/64, each straight,
    dotted (D) or as a triplet (T). Without a tempo from the host, it assumes 120 BPM. A new LFO starts synced, in
    quarter notes.
  - **One-Shot** runs an LFO through one cycle from each note and then holds it where the cycle ends, which is
    where it started (a one-shot is never free).
  - **Unipolar** makes an LFO swing from 0 to 1 instead of −1 to 1, so it only pushes its targets one way, like an
    envelope. It is the same choice the LFO's right-click menu offers.
  - **Random** takes a new value every cycle. **Chaos** sets how far it may move from the last one: up to a tenth
    of the range either way at 0 %, so it wanders, rising to anywhere at all at 100 % (the default), like sample
    and hold. **Smooth** glides to each new value instead of jumping: over that share of the cycle, easing in and
    out, then it holds (0 %, the default, steps).
  - **Custom** shapes are drawn from 2 to 16 points, starting as a triangle: up from the bottom to the top at the
    middle of the cycle and down again. Each segment between two points can be bent, from easing in
    to easing out, and the last segment wraps around to the first point, so the cycle loops without a jump. The
    points are saved with the patch (as hidden params, so hosts record edits for undo but don't list them).
- **Effects**, after the voices: everything the operators and filters send to the output runs through a chain of
  up to 16 effects. A new instance starts with an empty chain. Any effect can be added more than once, switched on
  and off, reordered and removed.
  - **Shaper**: picked from a list: Hard (a flat ceiling), Soft (tanh), Asymmetric (the lower half clips twice as
    hard, which adds even harmonics), Cubic (x − x³/3, scaled to reach full level), Gentle (x / (1 + |x|), which
    never quite gets there), Sine Fold and Fold (what goes past full level folds back, smoothly or in straight
    lines), or Custom, a curve you draw: points from the quietest input to the loudest, each segment bendable,
    level past the ends. A DC blocker takes out the offset that the asymmetric and drawn curves can leave. Drive
    (0–36 dB), Output and Mix; **Link** (on to begin with) brings the output down as the drive goes up, so
    turning the drive changes the tone more than the level: by as much as keeps a sine at −6 dBFS as loud (RMS) as it went in, dB
    for dB while the driven sine stays under the ceiling, then less and less as the clipping squares it off
    (about 8 dB at 12 dB of drive, 9 dB at the full 36). Louder sounds come out a little quieter, quieter ones a
    little louder. The curves are anti-aliased (first-order antiderivative), so high
    drive stays clean without oversampling, though folding at the highest drives gets duller for it.
  - **Delay**: separate times for the left and right side (1 ms–2 s each) or, synced, a note value of the host
    tempo for each (up to 4 s); Feedback (up to 95 %); Tone, a low-pass below the middle (20 kHz down to 200 Hz)
    and a high-pass above it (20 Hz up to 4 kHz), applied to every repeat; Width, from the echoes' full stereo
    down to mono; Ping-Pong, which starts the echoes on the left and bounces them across, each after its side's
    time; and Mix, where the dry signal stays at full level up to 50 % and the echoes reach full level there. Time
    changes glide like tape rather than click. The plugin keeps processing until the echoes have died away.
  - **DJ EQ**: a three-band isolator with Linkwitz-Riley crossovers (24 dB/octave) at 250 Hz and 2.5 kHz. Each
    band goes from kill to +6 dB, and with all three at 0 dB it is flat.
  - **Param EQ**: two bands, each with Frequency (20 Hz–20 kHz), Gain (±18 dB) and Q (0.3–10), and a shape:
    Peak (a bell), Low Shelf, High Shelf, Low Cut or High Cut (12 dB per octave, Q setting its resonance; the
    gain does nothing there).
  - **Filter**: the voice filters' types (low-pass, high-pass, band-pass, notch, comb; a row of icons on its
    card, with the list of slopes under the response) and their slopes, 6 to 24 dB, on
    the whole mix, with Cutoff, Resonance, **Key Track**, Drive and Mix. Key Track (0 to begin with) moves the
    cutoff with the newest voice's pitch, from C4 on, as a voice filter's does; once the notes end, it stays where
    the last one left it. Like the Param EQ, its display shows what it puts out behind its response.
  - **Compressor**: stereo-linked and peak-sensing, with Threshold (−60–0 dB), Ratio (1:1–20:1), a soft Knee
    (0–24 dB), Attack (1–500 ms), Release (1 ms–2 s), Makeup (0–24 dB) and Mix for parallel compression.
  - **3-Band Comp**: Linkwitz-Riley crossovers (Low X, High X) split the mix into three bands, each compressed on
    its own with its Threshold, Ratio and Gain after it (a 6 dB knee); Attack, Release and Mix are shared, and
    Output sets the level after the mix. At 0 % mix the bands just sum back up, so nothing combs. **Link** makes
    all three bands use the low band's Threshold, Ratio and Gain; each band keeps its own for when it is unlinked.
  - **Reverb**: a feedback delay network of sixteen lines (29 to 89 ms at full size, spread evenly on a log
    scale), mixed through a Hadamard matrix, which spreads each line's sound evenly over all of them every time
    round, so the echoes thicken fast. Each line's length wavers slowly (a fraction of a millisecond, each at its
    own pace between 0.3 and 1 Hz), so the tail's resonances smear instead of ringing on as a metallic tone.
    Each side of the input goes through the pre-delay and its own all-passes, then into half the lines, and the
    two sides of the output take the lines with signs that don't match, so the tail comes out wide even from a
    mono sound. Four algorithms: **Hall** (that network, four all-passes a side); **Room** (lines under half as
    long, a darker tail and tighter wavering, with six distinct early reflections that spread with the size);
    **Plate** (lines 60 % as long, six all-passes a side for a denser start, a brighter tail, little wavering); and
    **Shimmer**, a brighter hall that feeds its own tail back into itself pitch-shifted: four of its lines are
    read through pitch shifters as well (two read heads each, crossfading as they jump back across an 80 ms
    window), band-passed (200 Hz up to 8 kHz, which the damping lowers to 2 kHz), so each time round part of the
    tail climbs (or falls) by the interval, and the shifted part shifts again the next time. **Pitch** sets the
    interval, −24 to +24 semitones (an octave up to begin with), and **Shimmer** how much of those lines is
    shifted (50 % to begin with; at 0 it is a plain bright hall). Shifted a semitone or more, the crossfades keep
    the power of what they blend, so the tail lasts; within half a semitone, where the heads read much the same
    sound, they keep its level instead, so it can't build up. In the second after an A4 ends, its tail holds the
    octave about as loud as the note, and two octaves up at about a third of it. Size (the
    lines from a quarter of their length to all of it, gliding when turned, also when the algorithm changes),
    Decay (0.2–20 s to fall 60 dB), Damp (Damping in the host; the corner, from 20 kHz down to 1 kHz, above which the tail falls five
    times as fast, by as much in every line), Pre-Delay (off to 250 ms), Width and Mix (like the delay's: the dry
    sound stays at full level up to 50 %).
  - **Phaser**: 2–12 first-order all-passes (Stages) whose corner an LFO (Rate, 0–20 Hz; at 0 the sweep stands
    still) sweeps up to 2.5 octaves either way of Center (Depth), with Feedback (either sign) from the last stage
    into the first. **Sync** (off to begin with) makes the LFO go round once per note value of the host tempo
    instead, 4/1 down to 1/64 like an LFO's (a bar to begin with; 120 BPM without a tempo from the host). **Stereo** (off to begin with, under its display) runs the right side's LFO a quarter cycle
    ahead. Mix runs to 100 %, which is half wet: there the notches are deepest, and past it they would only get
    shallower again.
  - **Flanger**: a 1–15 ms Delay that an LFO (Rate, 0–20 Hz) sweeps by Depth, with Feedback of either sign (a
    negative one gives the hollower sound) and Mix, the right side a quarter cycle ahead. **Sync** works like the
    phaser's, two bars a sweep to begin with.
  - **Phase Disp** (phase disperser): 1–32 second-order all-passes in a row (Stages) at Freq, each as narrow as
    Pinch sets (Q 0.3 to 6): the sound around Freq comes out later than the rest, smeared in time into a chirp,
    but at full level; Mix blends it with the dry sound.
  - **Widener** (a dimension expander): the sound above **High Pass** (20 Hz–2 kHz, 150 Hz to begin with, 12 dB
    per octave), summed to mono, goes through four short delays, each swept slowly by an LFO of its own
    (0.29–0.53 Hz, 12 % of its delay either way). **Size** sets how long they are: 2–5 ms at 0, a tight doubling,
    up to 10–23 ms, a wide chorus. At full **Width** what the left taps have and the right ones lack is added as
    the sides, one side against the other, so the mono sum stays the dry sound; at 0 % all four are added in the
    middle, a chorus. **Mix** sets how much is added, the dry sound staying at full level. The lows below the
    high-pass stay in the middle as they were.
  - **Freq Shifter**: moves every partial by the same number of Hz, so harmonic sounds turn bell-like or beat
    slowly: **Shift** takes a share of **Range** either way, −100 % to +100 % (0 to begin with), and Range runs
    from 1 Hz to 5 kHz on a log curve (100 Hz to begin with), so a narrow one gives slow beating the knob's
    whole travel; Spread takes the right side the other
    way, Feedback sends the shifted sound round again for spiraling sweeps, and Mix. A pair of all-pass chains makes
    the 90-degree copy it needs, which keeps what is left at the old frequency around 50 dB down.

  The phaser and flanger keep their feedback from ringing louder: what they send round is scaled so their level
  across the spectrum stays where it was without it. Effect knobs can be modulated like any other. As the effects are shared by all voices, the LFOs and envelopes
  reach them through the most recently played voice. Each of the 16 places in the chain has the parameters of
  every effect type, and hosts only list the ones of the effect it holds. Each effect gets a remote control page in
  chain order.
- **Global**: **Volume**, the level of the whole output after the effects (−60 to +12 dB, 0 to begin with; the
  voices go into the effects at a level of their own, 12 dB down, so the effects' drive and thresholds don't
  depend on it; hosts modulate it globally, and LFOs and envelopes through the newest voice, as they do effect
  knobs), FM Amount, Pitch (±48 semitones, handy as an envelope target for drums), Pan, Bend Range for
  MIDI pitch bend, **Mod Wheel**, which moves what is assigned to it (nothing to begin with), and **Voices**, the
  polyphony limit (1–512, default 16).
- **Play modes**:
  - **Mono** plays one voice that moves from note to note. When you let go of the sounding key while others are
    held, it returns to the most recent one.
  - **Legato** (on by default, mono only): a note played while another key is down only changes the pitch, and the
    envelopes and LFOs keep running. Without it, every note restarts them from their current level, so there's no
    click.
  - **Glide** (0 = off, 1 ms–20 s) slides the pitch from the previous note, in mono and poly alike. By default it
    only glides between overlapping notes; **Always** makes it glide on every note. The slide is exponential and
    covers 99 % of the interval in the glide time. Glide can be modulated like any knob.
- **Host modulation**: all continuous parameters accept CLAP modulation, either globally or per voice (Volume
  globally only), so Bitwig's modulators (including per-note ones) can drive any of them directly.
- **Notes**: up to the voice limit, with quietest/oldest stealing (a stolen voice fades out over 3 ms instead of
  clicking), and CLAP note expressions (tuning, volume, pan). Voices are stereo all the way through the filters.
  Voices without any unison spread run their filters once, so they cost no more than mono voices.
- **Oversampling**: the voices (operators, shapers, filters and their drive) run at 1, 2, 4 or 8 times the host rate, 2 by
  default, and come back down through a polyphase IIR half-band decimator: flat to 20 kHz, no latency. This keeps
  bright FM, strong feedback and filter drive from aliasing. The effects run at the host rate. Levels, sends and
  routing amounts ramp sample by sample, and the ramps don't depend on how the host splits the block.

An operator or filter that is switched off is silent and costs no CPU. It is left out of the routing graph and
greyed out in the matrix, but its settings and connections are kept for when it comes back. A new instance starts with only operator A switched on,
as a plain sine carrier; the other operators share A's defaults (except that they don't go to the output), so
switching one on and wiring it in is the whole job.

## Editor

The editor is drawn with Direct2D and scales freely: hosts can resize it, and it keeps its aspect ratio. Collapsing
the modulation panel makes it shorter (the plugin asks the host to resize the window).

- **ROUTING / MATRIX / FX** tabs, under the header, pick what the panels below them show: the routing graph or
  the matrix (two views of the same connections) next to the inspector, or the effects chain across both. The
  choice is saved with the patch. While the shape, shaper curve or partial editor has the panels, no tab is
  highlighted, and clicking one closes the editor.
- **Routing panel**: the graph shows only what is switched on, the matrix everything, greyed out where it is off.
  The inspector's tabs switch nodes on and off (see below); so does a node's right-click menu in the graph.
  - **Routing** (graph): circles are operators (with their wave and ratio), rounded boxes the filters (with their
    type) and squarer boxes the shapers (with their curve). A colored outline means the node is heard, and it glows
    while it sounds. The nodes that are off wait in the graph's top left as dashed ghosts, a circle with an
    operator's letter or a box with a filter's or shaper's name; a click switches one on, back where it was in the
    graph, and shows it in the inspector.
    - Drag a node's **+** port onto an operator to modulate it, onto itself for feedback, onto a filter or shaper
      to send it there, or onto the **OUT** bus. An operator that already goes to the output is moved through a
      filter rather than doubled. A filter's port works the same way: onto an operator to modulate it, onto filter
      2 from filter 1, or onto the bus, and so does a shaper's, onto an operator, a filter or the bus; an operator
      or shaper that already goes out is moved through the filter it is connected to. Wires to or from a filter are
      purple, those to or from a shaper red, like the shaper effect.
    - A node's line to OUT ends in an arrowhead on the bus, like a wire's on the node it feeds, with its
      percentage label halfway down (beside the line, on its left, when the node sits too close to the bus for
      it). Drag an arrowhead onto another node to move the route there, or onto the bus to send its node out
      instead; the line to OUT's onto a node the same way. The route keeps its amount and its modulation, and
      follows the mouse until it is let go; it can't go where its node has a route already, or where a port
      couldn't take it, and let go of anywhere else, it stays where it was. An arrowhead's right-click menu
      disconnects its route. The feedback loop above an operator has no arrowhead.
    - Drag a wire's percentage label up or down to set the depth; double-click it to remove the wire, and
      Ctrl+Click it (or pick Edit value… from its right-click menu) to type a depth. A wire, or
      a node's line to OUT, turned down to 0 % stays, so modulation can still be dropped on it; only removing it
      (double-click, or Disconnect in its right-click menu) takes it away, together with its modulation. Dropping
      an LFO or envelope on a matrix cell or an Out knob shows that route's wire too, even at 0 %.
    - Drag nodes to rearrange the graph (saved with the patch). Click one to edit it.
  - **Matrix** (grid): each row is a source (an operator, filter or shaper), and its cells say how much of it goes
    to each column: the operators, F1 and F2, S1 and S2, and OUT. It always lists all ten nodes and fills the
    panel's height; the routes that can't be (a filter into a shaper or into the filter before it) are blank; the row and
    column of a node that is off are greyed out, its routes kept as they are. The diagonal is feedback. Drag cells
    vertically; double-click toggles a cell on or off; Ctrl+Click types an amount. Dragging a cell, or toggling it
    on, switches on the nodes it joins. A row's or column's name works like the node's tab in the
    inspector: clicking it shows that node there, switching it on if it is off, and the names of the node shown
    sit on a plate, like its tab, where a click switches it off, or on again.

  - **FX**: takes the place of both the routing panel and the inspector. It shows a card per effect, left to right
    in processing order, each in its type's color (shaper red, delay blue, DJ EQ amber, Param EQ orange, filter
    purple, compressor green, 3-band teal, reverb indigo, phaser pink, flanger lime, frequency shifter cyan,
    phase disperser slate, widener fuchsia) and
    with its icon, which switches it on and off, an × to remove it and a live display:
    - the shaper's transfer curve; a Custom curve shows its own points and bends instead, to drag right there
      (double-click adds or deletes a point, Ctrl snaps to the grid), with what the drive, output and mix make of
      it faintly behind and the point under the mouse in numbers; its EDIT button opens the big editor;
    - the delay's echoes on a timeline, left above the line and right below it;
    - the EQs' and the filter's responses; behind the Param EQ's curve, what it puts out, measured (a 2048-point
      FFT in 96 bands from 20 Hz to 20 kHz, 0 dB at the top, falling away at 55 dB a second), which it only
      measures while the editor is open;
    - the compressors' curves (input level across, output up) with a gain reduction meter; the 3-band card's
      LOW / MID / HIGH buttons pick which band's curve is on top and which band its first row of knobs sets
      (a thin line divides those from the shared knobs), and LINK beside them gives the bands one curve;
    - the reverb's tail: after the pre-delay it builds up (slower in a bigger space, with its early reflections
      spread wider) and falls to nothing at the decay time, its texture sparser the bigger the space; a room's
      early reflections stand out, a plate is dense from the start, and a shimmer's shifted tail sparkles above it
      (below it for a shift down), each step of the interval later, further out and fainter, the less it shifts
      the fainter, with the interval over the display. Its algorithm comes from a list under the card's title,
      like the shaper's curves; a shimmer's card has two rows of four knobs, its Pitch and Shimmer among them;
    - the phaser's and flanger's responses faintly at both ends of their sweep, and bright where their LFO is,
      moving while the audio runs (a stereo phaser's right side as a second, thinner curve);
    - the phase disperser's delay across frequency, peaking at Freq;
    - the frequency shifter's move: a 220 Hz tone's partials, and where it takes them on each side, under the
      shift in Hz and the range it is a share of;
    - the widener's stereo field: left to right across, frequency up (20 Hz to 20 kHz), the dry sound a line
      down the middle and the sides it adds spreading out above the high-pass (a dashed line), as far as Width
      and Mix take them, wavering with the sweep while the audio runs; what Width leaves in the middle thickens
      the line. Above it, the high-pass and the taps' delays.

    The delay's switches above its display are icons across the card, named on hover: a metronome for Sync, two
    chain links for Link (the right side follows the left) and a ball between two paddles for Ping-Pong. The
    shaper's Link, over its Output knob (which it ties to the drive), is the same chain; its curve's list takes the
    whole row above the display. Every effect with a Sync
    has that metronome: the phaser and flanger at the left under their displays (the phaser's STEREO beside it),
    over their Rate knob, which, synced, picks a note value and shows it as its label, as the delay's time knobs
    do. The filter's slope is a list under its display, like a voice filter's (see the inspector). The filter's
    knobs come in two rows, Cutoff, Reso and Key Track above Drive and Mix, and so do the
    frequency shifter's, Range, Shift and Spread above Feedback and Mix.

    Each effect has an icon, at the left of its card's title and in the list of effects. On the card, the icon
    is the effect's switch: a click turns it off (the icon turns grey with the card, under the mouse too, and the
    effect costs nothing) or on again, in its color; dragged, it moves the card like the title. **ADD EFFECT** at the panel's top left
    opens a list of the effect types (beside it, how many of the 16
    places are taken, as in 3/16), and so does a dashed card with a plus after the last effect, over itself; the
    one picked goes at the end of the chain, which scrolls to show it, and so does a copy. The dashed card takes a
    card place at the chain's end while there is room for another effect. Six cards fit across, the Param EQ's
    taking one and a half places for a wider spectrum; with more,
    a scrollbar under them (drag its thumb or click its track) or the mouse wheel over the panel scrolls the chain
    smoothly, cutting off the cards at the ends. Drag a card by its
    title to move it along the chain,
    and right-click the title (or the icon) to duplicate the effect (with its settings), reset its settings or
    remove it. The chevron left of a card's × folds it to a narrow strip, as wide as the icon's place at the
    title's left, so the icon stays where it was: the icon, which still switches the effect and drags the card,
    its ×, then its name down the strip, which unfolds it. The cards after it close up; folds are saved with the
    patch but not undone. On the Param EQ's display,
    drag a band's numbered handle for frequency and gain, Alt-drag it up or down (it stays where it is) or scroll
    on it for Q, and double-click it to flatten it; a cut's handle sets its resonance instead of a gain, and a
    double-click takes the resonance away. Each band's
    row of knobs starts with its shape, an icon in the band's color with its name under it, which opens the list
    of shapes.
  - **Partial editor**: picking Custom from an operator's **…** list opens its partials over the routing panel:
    the wave they make, one cycle, then a bar per partial for its level and, in another color, one for its phase
    (a full cycle at the top, faint while the partial is silent). Drag across the bars to draw them, double-click
    a bar to take it to nothing; hovering shows a partial's numbers. RESET goes back to a sine, DONE (or a tab)
    closes it, and Custom from the list opens it again. A stroke is one undo step.
  - **Shape editor**: picking an LFO's Custom shape (or clicking its display, which then shows EDIT) opens the
    shape over the routing panel and the inspector, which it hides while it is open; DONE closes it, and RESET puts back the shape it started with. Drag points to
    move them; double-click empty space to add a point and a point to delete it (or right-click it). The small
    dots halfway along each segment bend it: drag them up or down, double-click to straighten. **GRID**, under the
    editor, sets how many lines run across (2–32, thirds included) and up and down (2–16), with its − and +
    buttons or the mouse wheel; hold Ctrl to snap points to them. The grid belongs to the editor: it is saved with
    the project, and presets leave it alone. The first point always sits at the start of the cycle. A shaper
    effect's Custom curve opens the same editor from the EDIT button on its card's display, in a panel in the middle of the
    effects' (the inspector stays hidden), its plot closer to square, with input across and output up; its first
    and last points stay at the ends. The note source's curve opens over the routing panel from its display in the
    keyboard strip (a second click closes it): the notes from Low to High across, the Cs among them named under
    the plot, and what the source puts out up, from nothing at the bottom to all of it at the top, with a dot
    where the newest voice is while notes play. A dashed line shows, for the range set, what would take a cutoff
    at 0 by 100 % onto the note. **Low** and **High** are knobs side by side in its title row, dragged in whole
    notes (or typed, as `C4`, `F#2 -20` or a MIDI note number); its ends stay at the ends too, and RESET brings
    back every MIDI note and the straight line.
- **Header**: the output meter, left of Volume: a bar for each side showing its peak, from −48 dB at the bottom,
  in the theme's color up to full scale and red past it, falling 24 dB a second (also once the plugin stops
  processing); the strip above full scale lights up red for a second and a half after the output went over. Then
  the global knobs; the play cluster (Poly/Mono, Legato, Glide and its Always switch, each switch
  explaining itself on hover); Bend; Voices (it drags on a log scale, so 4 and 512 are both easy to reach); and
  how many voices are playing.
- **Undo and redo**: the curved arrows left of the preset buttons step back and forward through the patch's
  edits, up to 100 steps; hovering one says what it would undo. A drag, a run of wheel notches on one knob, a typed
  value, adding or removing an effect or source (with its modulation), moving nodes or cards, Init and loading a
  preset are each one step. The history lives in the plugin, so it outlasts closing the editor, and goes when the
  host loads a saved state. **Ctrl+Z** undoes, and **Ctrl+Y** or **Ctrl+Shift+Z** redoes, while the mouse is over
  the editor and its window is the active one: a keyboard hook catches them before the host does, so the host's
  own shortcuts keep working everywhere else.
- **Inspector** (right): tabs A–F, F1 and F2, and S1 and S2 pick what it shows; tabs of switched-off nodes are
  dimmed, and clicking one switches its operator, filter or shaper on. A dot in a tab's top right corner says the
  node's own sound goes out: an operator's or filter's Out is up (a filter's only while something goes into it), or
  a shaper, which can't go out itself, feeds a filter that does. Clicking the selected tab switches it off, or on again (its
  tooltip says which). The power switch next to the summary line does the same. While the node is off, its page
  is dimmed below the summary line; its knobs still work.
  - Operators: wave buttons for the common waves, then **…**, which lists Pulse, Half Sine, Abs Sine and Custom (picked,
    one of these shows on that button); ratio, pitch, fine, detune, velocity and output, the unison knobs and Rnd Phase. A small fan display
    shows where the copies sit (left/right by spread, up/down by detune). A noise operator shows Slope and Spread
    instead of the pitch and unison knobs, and its spectrum against white, pink and brown; its circle in the graph
    is labeled with its color instead of a ratio. The envelope is a curve with draggable
    points: the peak (attack), the end of the hold just after it, the sustain level (decay and sustain) and the end
    (release). The small dot halfway along the attack, decay and release bends that segment (drag up or down,
    double-click to reset). The display always zooms to fit the whole envelope across its width, so the handles of
    short segments stay apart; the zoom holds still while a handle is dragged, so it stays under the mouse, until
    the envelope would run past the display's end: then it zooms out to keep all of it in, the handle at the end,
    and dragging on lengthens the segment further. The same goes for the mod envelopes' displays. In the graph, an operator with unison is drawn as a stack of rings.
  - Shapers: the summary (its curve, what feeds it, the filters it feeds, whether it goes out and the operators it
    modulates), its curve's list with the link switch, a chain, beside it, the Drive, Output, Mix and To Out knobs,
    and its transfer curve; a Custom curve shows its points and bends there to drag, like a shaper effect's card, and its
    EDIT button opens the big curve editor over the routing panel, the shaper's page staying beside it. Underneath
    are the send knobs of the operators that feed it.
  - Filters: type buttons, each a small picture of its response (named on hover), and the slope's list at the end
    of their row, 6, 12, 18 or 24 dB (a band-pass's or notch's shows 6 and 18 faintly and can't pick them, and shows
    what plays; for a comb it is faint and doesn't open), cutoff, resonance, key tracking, drive and output, and a live response curve whose
    handle drags cutoff (sideways) and resonance (up/down). With its cutoff or resonance modulated, faint curves
    show how far the modulation takes it either way, and while notes play the curve follows where the newest
    voice is (the handle stays at the set values, the cutoff a C4 has). Key tracking moves all of it with that
    voice's note, an octave per octave at 100 %. The filter effect's display does the same, its key tracking
    staying where the last note left it, as the effect's does. Behind the curve,
    like the filter effect's, is what the filter puts out, all voices together, measured before its output level
    (the Param EQ's analyzer: a 2048-point FFT in 96 bands, 0 dB at the top), while the editor is open; a filter
    that is off shows none. Underneath are the send knobs of everything that feeds the filter.
- **Modulation**: the panel's title row has the **MODULATION** toggle, which collapses the panel to that row and
  opens it again (the editor gets shorter; saved with the project, but not part of presets), then **ADD LFO** and
  **ADD ENV** to add a source (which opens a collapsed panel). Below, the sources' cards show four at a time, and
  after the last one, while there is room for more, two square dashed cards with a plus, ADD LFO above ADD ENV,
  add one too. With more than fit, the scrollbar under them or the mouse wheel over the panel scrolls them
  smoothly. LFOs and envelopes are numbered by type in the order they show.
  - Drag a card by its title to move it along; the order is saved with the patch. Right-click the title (or the
    handle) to duplicate the source (with its settings and targets), clear its targets, make it **Unipolar** or
    **Bipolar** (the one in force checked) or remove it; the × removes it too, together with its modulation and
    whatever modulates its knobs. A bipolar envelope's display has a dashed line at its middle level and ± in its
    corner.
  - The power switch turns a source on and off; one that is off has its card dimmed and its handle and its rings
    on knobs faint (the handle can still be dragged onto targets).
  - The chevron left of the power switch folds the card to a narrow strip, as on the effects: the handle, a grip
    to drag it by, the switch and ×, then the name, which unfolds it.
  - Each source has a colored handle in its card's title row. Drag it onto any knob, wire label
    or matrix cell; a filter's cutoff is the classic target. Held near the left or right end of the modulation
    panel or the effects chain, it scrolls those cards that way, faster nearer the edge, so a knob on a card out of
    view can be reached. Hovering the handle draws a dashed line to each of its targets on screen, ending at the
    small dot it has there (see below), with a ring round the dot. A random
    LFO has **Smooth** and **Chaos** knobs beside its display, which shows four made-up cycles of what they do;
    while notes play it shows the LFO's own last four cycles instead, swept across: each new cycle takes the next
    quarter, round from the right end to the left, the older ones fainter, with the live dot on the newest (at
    the end of a cycle, it waits there until the next one comes in). An LFO's sync switch, the metronome the
    effects' SYNC has, sits under its Rate knob and turns it into a note-value knob (as it starts, on). Left of it,
    under its shapes, three more switches drawn as icons (named on hover), each under two shapes: an infinity sign
    for **Free**, which stops notes from restarting it, a single cycle that ends in a dot for **One-Shot**, and
    humps on a baseline for **Unipolar**. A unipolar LFO's display has its zero line at the
    bottom.
  - A new modulation starts at +30 %, upward, whatever the target's setting. The target then shows a colored ring
    for the modulation range, plus a small dot of the same color under the
    knob (on a wire label or matrix cell, in its top right corner). Drag that dot to set the depth; double-click or
    right-click it to remove the modulation. A source can modulate another source's depth too (drop its handle on
    a depth bar in that source's list): the bar then has the dot in its top right corner, a strip under it shows
    how far the depth swings, and hovering the handle draws a dashed line to it like to any other target.
  - **Bypass**: the dot at the start of each target's row in the source's list switches that modulation off and
    on again, keeping its target and depth; so do **Bypass** / **Resume** in the right-click menus of the row, the
    knob's dot and the modulated knob. A bypassed modulation's dots are hollow, its row and ring dimmed. Bypasses
    are saved with the patch (as hidden params, like the drawn shapes).
  - Each source lists its targets with a depth slider and a remove button, four at a time; with more, a thin
    scrollbar beside them scrolls the list smoothly (drag its thumb or click its track), and so does the mouse
    wheel over the names, gliding a row per notch (a trackpad moves it by parts of a row); the rows at the list's
    ends are cut off on the way.
  - While notes play, a white dot on each modulated knob shows where the newest voice currently is.
  - A modulated route shows it too: its wire (or its line to OUT) has a thin core in the source's color and
    swells and thins with the modulation while notes play, and its label or matrix cell has a small bar along
    its bottom with each modulation's range, a tick where the amount is set and a white dot where it is now.
- **Everywhere**: drag up/down to change values (hold Shift for fine steps; sliders such as the depth bars, the
  Mod wheel and the glow slider then move from where they are, a tenth as fast), double-click to reset, use the mouse
  wheel, or right-click for reset/remove options. Double-click the number a knob or a target's depth shows (under
  the knob, or beside it in the header), or pick **Edit value…** (**Edit depth…** on a modulation) from the
  right-click menu, or Ctrl+Click it, to type a value, in the units it shows or as a plain number (`300` on a
  time is 300 ms, `1.2k` on a cutoff 1.2 kHz); Enter or a click elsewhere sets it, Escape drops it. The editor
  takes the keyboard while you type and hands it back afterwards; the keys are taken straight off the window's
  message queue, so hosts whose message loops pass keys on to themselves (Bitwig) don't get them first. A wire
  label, OUT label or matrix cell types a value on Ctrl+Click, since its double-click removes or toggles the route.
- **Keyboard strip**: along its top the note source, the macros and the octave shift; under them the wheels and
  six octaves of virtual keys, from C1 to C7 (the − and + buttons at the top right shift them by an
  octave). Drag across the keys to glide between notes. Keys light up while they are held, whether clicked here or
  played by the host. The **Bend** wheel bends the pitch by the Bend Range and springs
  back to center on release; the **Mod** wheel is the Mod Wheel param, and the handle right of it drags onto what
  it should move, like a macro's (faint, like theirs, until it moves something). The wheel's and its handle's
  right-click menus make it unipolar or bipolar; bipolar, it fills from its middle, where it moves nothing.
- **Macros** (in the keyboard strip, above the keys): eight knobs, each in a color of its own, with a handle
  beside it to drag onto anything a source can modulate, just like an LFO's, and its name under it: double-click
  the name to change it (Enter
  keeps it, Escape doesn't, an empty name brings back "Macro 1" and so on; an undo step, saved with the patch).
  Hovering a handle draws lines to what it moves; the dots and rings on its targets work like a source's. Until a
  macro modulates something, its handle, its knob's arc and its name are faint. The right-click menu of its knob,
  handle or name makes it **Unipolar** or **Bipolar** (and the handle's or name's clears its targets); a bipolar
  macro's arc starts from the knob's middle, where it moves nothing.
- **Note** (in the keyboard strip, left of the macros, above the wheels): the note source's handle, to drag onto a
  target like a macro's (and like a macro's, faint until it modulates something), and beside it the note the newest voice plays, as the source follows it: its name, and
  the cents it is off while bent or gliding (a dash while nothing plays), over a faint picture of its curve. A
  click on it opens the curve's editor (see the shape editor). Its right-click menu, and its handle's, make it
  unipolar or bipolar (± in the display's corner).
- **Presets**: presets are `.fmv` files in `Documents\FMVibe\presets` and its subfolders. The first time the
  library is used (the browser, the arrows or a file dialog), FMVibe makes that folder with its examples in the
  `Examples` subfolder: Electric Piano, FM Bass, Glass Bell, Warm Pad, Drawbar Organ, Pluck Delay; Crystal
  Motion, which uses everything at once (all six operators in three layers, two filters, four LFOs and a mod
  envelope, and six effects); Reese Bass, a heavy drum and bass reese (two stacks of detuned saws through a
  driven low-pass, a clean sub, clipping and three-band compression); Wobble Bass (an eighth-note LFO throwing
  open a resonant filter and the FM together); Shimmer Pad (unison layers through two filters, three LFOs and the
  shimmer reverb); Trance Gate (supersaws chopped in sixteenths by a square LFO, a filter sweep over two bars);
  FM Brass (two stacks that blat in with velocity, an envelope on the FM, vibrato); Industrial Hit (a body
  dropping two octaves, band-passed noise, metallic partials and a sine-fold shaper); Folded Lead (a mono lead
  whose modulator goes through a sine-fold shaper on its way to the carrier, driven harder as each note starts);
  and Fuzz Bass (an FM pair through an asymmetric shaper into a plucked low-pass, the shaper also growling back
  onto the modulator, over a clean sub). A folder made before an
  example came gets it the next time the library is used; `%APPDATA%\FMVibe\settings.txt` counts the examples
  given, so deleted ones stay gone.
  - The **preset bar** in the middle of the tabs' row shows the preset loaded or saved last, or **- Init -** (as
    a new instance does), with a dot while the patch has changed since. Its arrows step to the previous or next
    preset in the folder, round from the last to the first. Clicking the name opens the **browser**: the presets
    at the top of the folder, then each subfolder's under its name, the current one marked; a click loads one
    and closes the browser (if the patch has changed, it asks first, over the browser, which stays open when the
    load is called off); **OPEN FOLDER** shows the folder in Explorer, and the × or a click around it closes it.
  - **Init** sets every param and the routing layout back to their defaults (the zoom stays). **Load** and
    **Save** read and write a `.fmv` file through the standard file dialog, which starts in the preset folder;
    Save suggests the current preset's name. The file holds the same data as the host's saved state, apart from
    the settings.
  - Init and loading ask first only while the patch has changed since it was loaded, saved or reset, or when the
    host brought a patch that isn't a preset; undo brings a replaced patch back, with the preset bar's name and
    its changed dot as they were (undoing every edit since a preset loaded leaves it unchanged again), and redo
    the same forward. The project remembers which preset it was and whether it changed.
- **Settings** (the cog after Save): **Oversampling**, 1x, 2x, 4x or 8x. It is saved with the project but not
  with presets, and changing it restarts the plugin (the host deactivates and reactivates it).
  **Theme**: Midnight (the default), Graphite (neutral grays), Light (crisp: white cards and knob caps on a cool
  gray, with deep colors) or Paper (warm: cream on sand, with ink-brown text and earthy colors), each button
  shown in its own colors. **Glow**, a slider from Off to 100 % (50 % to begin with), makes the curves in the
  graphs (envelopes, LFOs, filter and effect responses) glow, drawing them a little thinner: the higher, the
  stronger and wider the halo.
  The editor shows it while it is dragged and keeps it when it is let go. Both are yours rather than the
  project's: they are kept in `%APPDATA%\FMVibe\settings.txt`, every FMVibe editor uses them, and other open editors
  switch along.

### Remote-control pages (Bitwig)

| Section | Pages |
| --- | --- |
| Mix | Macros (the eight knobs, the page shown first), Global (Volume, FM Amount, Pitch, Pan), Play (Mono, Legato, Glide, Glide Always, Bend Range, Voices), Outputs (A–F, then S1 and S2 To Out) |
| Operators | Op A … Op F: Wave, Coarse, Fine, Detune, Attack, Decay, Sustain, Release |
| Filters | Filter 1 and 2: Type, Cutoff, Resonance, Key Track, Drive, Out, On, Slope; Filter links: F1 → F2 |
| Shapers | Shaper 1 and 2: On, Curve, Drive, Output, Mix, Link, then what it feeds into F1 and F2 |
| FX | One page per effect in the chain, named after its place and type, with that effect's knobs (a shimmer reverb's Pitch and Shimmer in place of its Pre-Delay and Width) |
| Matrix | "A modulates" … "F modulates": what that operator modulates; "Into F1", "Into F2": each operator's send into that filter; "F1 modulates", "F2 modulates": the operators each filter modulates; "Into S1", "S1 modulates" and the same for S2: each operator's send into that shaper, and the operators it modulates |
| Modulation | One page per source, in the editor's order: an LFO's shape, rate, sync and division, or an envelope's attack, decay, sustain and release, then its 4 slot depths |
