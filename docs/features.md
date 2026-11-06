# FMVibe features

What the plugin does and how its editor works, in full. Building and installing it are covered in the
[README](../README.md), working on the code in [AGENTS.md](../AGENTS.md).

## Sound engine

- **Operators A–F**, each of which can be switched on or off, and each with:
  - a **wave**: Sine, Triangle, Saw, Square or Noise, or one of the less common Pulse (25 %), Half Sine, Abs
    Sine (an octave up) and Alt Sine (a whole sine in the first half of the cycle, then silence: a strong 2nd
    harmonic over the fundamental), the last three like the classic FM chips'; **Organ** (drawbars 8', 4', 2 2/3',
    2', 1 1/3' and 1' pulled to 8 8 6 6 4 4); **Vowel** (a buzz falling 6 dB an octave through formant peaks at
    the 7th, 11th and 24th harmonics, an "ah" around 100 Hz that turns brighter or duller as the note rises or
    falls); or **Custom**: a wave drawn from its first 16, 32 (to begin with) or 64 partials, each with a level and
    a phase (picked after another wave, the partials start as that wave's, all 64 of them; otherwise a plain sine).
    Fewer partials leave the ones past them out of the sound, but keep their levels and phases for when there are
    more again. It is band-limited like the others and normalized to its
    peak; the synth builds its tables again whenever the partials change. Noise has no pitch or unison: instead a **Slope** moves it from white through pink (−3 dB per
    octave) to brown (−6 dB per octave) at about the same loudness, and **Spread** (the Uni Spread param) widens it
    from mono to fully uncorrelated left and right;
  - **Ratio**, 0.50 to 64.00 times the note, set to the hundredth. It is stored on a log curve, so a host's knob
    gives 1 to 2 as much travel as 32 to 64, and a modulation's depth is the same interval wherever the ratio is set
    (a seventh of the range is an octave); modulated, it glides continuously, and goes on past 64 up to 128. Patches saved before it (state version
    4 and earlier), which had a stepped Coarse (0.5, 1…32) and a Fine adding up to +100 %, load their ratio from those
    as they played, and a modulation on either moves onto Ratio with the depth that reaches the ratio its full swing
    reached from where the ratio is set. Each operator's pitch moves to a new control block's value over the block's
    length (not in a step at its start), for Ratio and every other pitch modulation alike. **Pitch** (±48 semitones on top of the
    ratio; the knob moves in whole semitones, while LFOs, envelopes and host modulation glide through it), Detune
    (±50 cents) and an AHDSR envelope (by default an organ-like gate: no hold, full sustain and a 15 ms release;
    its attack, decay and release go from 0 ms to 10 s, through fractions of a millisecond (0.1 ms, 0.2 ms …) below
    1 ms, its hold from 1 ms, and the hold can be Off; at 0 ms a segment takes a single sample, so a 0 ms attack starts a note at
    full level at once, nothing else ramping a new voice in (in the envelope's display, an
    attack's segment shrinks away below 1 ms, so a 0 ms attack is a vertical line, and a decay's or release's to half
    its shortest width), and after a 0 ms decay a changing sustain is still followed over 1 ms, so it doesn't step.
    The knob's stretch from 0 to 1 ms lies below where it used to stop, so patches saved before it (state version 3
    and earlier) keep their times, and their modulation depths on these times load scaled to swing them as far as
    before);
  - **Fixed** pitch (off by default): the operator plays at **Freq** (1 Hz–2 kHz on a log curve) times **Mult**
    (×0.001, ×0.01, ×0.1, ×1 or ×10), so 1 mHz to 20 kHz, whatever the note. The note, pitch bend, glide, the
    global Pitch and the note's tuning don't move it, nor do its ratio, Pitch and Detune, or modulation on them;
    its unison copies still spread about it. Freq can be modulated (a pitch sweep on a fixed operator is a mod
    envelope on it); Mult, a few steps, can't. Below about 20 Hz a fixed modulator gives a wobble that ignores
    the note;
  - **Hold**: every envelope (operators and mod envelopes alike) can stay at full level for a while after its
    attack, before the decay starts (off by default);
  - **Envelope curves**: the attack, decay and release of every envelope (operators and mod envelopes alike) can
    each be bent from −100 % (starts slow, rushes at the end) through 0 (a straight line) to +100 % (starts fast and
    settles). Every segment arrives in its set time, whatever its curve. By default the attack is straight and
    decay and release sit at +50 %, an exponential fall of 60 dB over the segment's time.
  - Out (level straight to the audio output) and sends into the two filters and the two shapers;
  - **Phase**: where a note starts the waveform, 0 to 100 % of a cycle (0 % by default; a sine at 25 % starts at
    its peak). It is read as a note starts, with the host's and the sources' modulation on it then, and holds for the
    note. A noise operator has none. Operators don't follow velocity themselves: velocity reaches them as a source
    (see Velocity, under the modulation sources), aimed at Out or wherever else;
  - **Unison**: up to 16 copies of the operator. *Uni Detune* spreads them in pitch (the outermost copies by up to
    ±100 cents) and *Uni Spread* across the stereo field. On a modulator, the copies' sum is what its targets
    hear; on a carrier, every copy is modulated by the same signal.
  - **Rnd Phase** (phase randomization): at 0 % (the default) every note starts the waveform, and each unison copy, at
    Phase, so notes sound alike from the first sample; turned up, each copy starts up to that much of a cycle
    later, at random. Unison copies at 0 % start in step and drift apart as they detune.

  Waves come from band-limited wavetables with one table per octave, so saws and squares stay clean high up the
  keyboard, even when they are modulators.
- **Matrix**: `X → Y` sets how strongly operator X modulates operator Y, and `X Feedback` is X modulating itself.
  Any operator can feed any other, cycles included. In each sample, earlier operators (A before B) pass on their
  current output and later ones their previous output; feedback is smoothed over two samples. Amounts are squared
  internally, so the low end is fine-grained. **FM Amount** scales the whole matrix.
- **Filters F1 and F2**, per voice, each switchable on and off (a new instance starts with both off). Any
  operator or shaper can be sent into either filter, and each filter can feed the other one (`F1 → F2` for a
  series chain, `F2 → F1`) and itself (`F1 Feedback`), and either can feed the shapers (`F1 → S1`, see below); each filter has its own output level, its **Out**, which, like a shaper's or operators B–F's, is 0 to
  begin with, so a filter is heard only once it is routed out (see the routing panel). A filter's
  output can also modulate operators, the same way an operator does (`F1 → A` and so on, scaled by FM Amount):
  noise through a band-pass, for example, gives a carrier a narrow band of grit. Operators hear the filter's
  previous output sample. Filter 2 hears filter 1 as it is that sample; filter 1 hears filter 2, and each filter
  itself, a sample late, so filters can feed each other and themselves round in a loop. Once a released note's
  operators have faded, those loops fade away, so they can't ring on, and what a filter hears from itself or a later
  filter is held within +18 dB, so a loop that lifts itself past unity grows loud but never without end. A filter
  that only modulates can leave its output level at 0. Types:
  - Low-pass, High-pass, Band-pass and Notch: a state-variable filter that stays smooth under fast modulation.
    **Slope** picks 6, 12 (the default), 18 or 24 dB per octave. At 6 dB the filter is a one-pole, taken from the
    state-variable filter so resonance still adds a peak at the cutoff (as high as at 12 dB) while the slope past it
    stays 6 dB per octave; 18 and 24 add a second, plain stage after the resonant one (a one-pole, or a
    Butterworth), so the resonance stays a single peak. A band-pass or notch comes in 12 and 24 dB only: set to 6 or
    18, it plays the slope above;
  - Comb: a tuned feedback delay, so the cutoff sets the pitch and resonance the feedback. With its cutoff at 0
    and the note source on it at 100 %, its curve drawn along the cutoff knobs' (and Key Track left at 0), it
    rings at the pitch of the note.

  Every filter's Cutoff, the filter effect's too, starts (and resets) at its knob's middle, about 632 Hz: there,
  a bipolar note source on it at 100 % puts it right on the note. Each filter also has Resonance, Key Track (how
  far the cutoff follows the note from C4 on, an octave per octave at 100 %; 0 to begin with, on every filter) and
  Drive (saturation before the filter, anti-aliased like the shapers). A voice only ends once its filters have
  rung out, and a filter whose sends go to 0 rings out too; when they come back, or its type or slope takes up a
  stage it left out, that starts from silence.
- **Shapers S1 and S2**, per voice, each switchable on and off (off to begin with): the shaper effect in every
  voice, with its curves (Hard, Soft, Asymmetric, Cubic, Gentle, Sine Fold, Fold, or a Custom curve drawn like the
  effect's, the S it starts as included), its Drive (0–36 dB), Output, Mix, Link (on to begin with) and Symmetric
  (off), and run by the same code: anti-aliased the same way, a drawn curve gliding to moved points and its level at the middle taken out the same way, with the same DC blocker after the uneven and drawn curves. Any operator,
  filter or shaper can be sent into a shaper (`A → S1`, `F1 → S1`, `S1 → S2`), a shaper into itself too (`S1
  Feedback`), and a shaper's output feeds the filters (`S1 → F1`), for
  distortion into a filter or a filter's sound distorted, and modulates operators the way an operator does
  (`S1 → A`, scaled by FM Amount), so a modulator can be folded or squared off before it reaches its carrier. Its
  **To Out** sends it straight to the output, like an operator's Out (0 to begin with, so a new shaper is heard only
  once it is routed out, or through what it feeds or modulates). Operators hear its previous output sample, and it
  hears a filter's previous one, so a filter and a shaper can feed each other round in a loop. S2 hears S1 as it is
  that sample, and S1 hears S2, and each shaper itself, a sample late, so shapers can feed each other and themselves
  round in a loop too. Once a released note's operators have faded, what the shapers get from the filters and the
  shapers fades away, so such a loop can't ring on. What a shaper hears from a filter or a shaper is held within
  +18 dB, so a loop that lifts itself past unity (a resonant filter, or shapers feeding back, with a shaper's Mix
  below 100 %) grows loud but never without end. It runs only while something goes in and its output goes
  somewhere (a shaper feeding only itself goes nowhere; one feeding a shaper that goes somewhere does).
- **Modulation**: up to 16 sources, any mix of LFOs (sine, triangle, saw, square, random or a freeform shape;
  0.02–100 Hz), mod envelopes (AHDSR, starting at full sustain and with an operator's 15 ms release; times as an
  operator's envelope has them), and two that follow what a note or the controller brings:
  - **Random**: a value of its own for each note, taken at random as the note starts (or restarts, in mono without
    legato) and held, 0–100 %: notes that each detune, brighten or pan a little differently.
  - **MIDI**: what its **Input** picks. **CC**, to begin with: a MIDI controller, **CC** 0–127 (1, the mod wheel's,
    to begin with), the same for every voice. **Pressure**: aftertouch, a voice following its own note's (MIDI
    polyphonic aftertouch, CLAP's pressure note expression, or channel pressure on an MPE member channel) once some
    has come, and the channel's (MIDI channel pressure) until then. **Slide**: a voice's own note's slide, the
    finger's up and down movement an MPE controller sends (CLAP's brightness note expression, or CC 74 on an MPE
    member channel), resting in the middle until some comes: bipolar, it moves nothing at rest. It follows its
    input through **Slew**, a time constant from 1 ms (all but at once, to begin with) to 10 s, starting each note
    where its input is.
  Both start unipolar. The browser demo takes no MIDI, so there a MIDI source stays at 0. A new
  instance has none; each one added starts
  switched on, and a source that is switched off stands still and modulates nothing, but keeps its targets. Each
  has 16 slots, and a slot can
  target any continuous parameter: a knob, a matrix amount, an operator output, even another LFO's rate or a slot
  depth. Depth is ±100 % of the target's range: how far a **unipolar** source takes the target one way, and the
  whole swing of a **bipolar** one, half of it below the set value and half above (at 100 %, −50 % to +50 %).
  Every source can be either, for all of its modulations, picked from its right-click menu or the switch after its
  name in the MODS view (see the editor): LFOs start bipolar, and envelopes, macros, the note source, the mod wheel
  and velocity unipolar. A bipolar envelope swings about its middle level (nothing at half of it, half the depth
  down at rest and up at its peak), and a bipolar macro, mod wheel, note source or velocity about the middle of
  what it puts out (a macro moves nothing with its knob at 50 %). LFOs and envelopes run
  per voice and restart
  with each note. LFOs and envelopes see each other's slot modulation one 32-sample block later.
  Each modulation has a **curve** (-100 to +100 %, a straight line to begin with), which bends what its source puts
  out before the depth takes it, as an envelope segment's curve bends the segment: above 0 it rises fast and
  settles, below 0 it starts slow and rushes at the end, and the ends stay where they are. A bipolar modulation's
  curve bends each half from the middle out alike, so the middle of the swing still leaves the target at its set
  value. One source can so open a cutoff in a straight line and swell an operator's level slowly at once.
  Each modulation also has an **Amount** (0–100 %, all of it to begin with), which scales its depth, and can have an
  **aux**: another source whose output scales it too, as a unipolar source puts it out (0 to 1), so a macro can
  fade an LFO's vibrato in, or velocity open an envelope's sweep wider. Set either in the MODS view (see the
  editor); the aux also by dropping a source's handle on the modulation's depth handle, or on its aux cell in the MODS view. An aux that runs per voice
  (an LFO, an envelope, the note source, velocity) scales it in each voice by its own output there, as of the last
  32-sample block (or this one, for a source that comes before it, and velocity, known from the note; on a note's
  first block the note source's comes from the pitch the note starts at); a macro or the mod wheel scales it the same for
  all. A macro's or the mod wheel's modulation with an aux that runs per voice then runs per voice too, reaching the
  effects through the newest voice. A switched-off aux puts out nothing, so it scales its modulation to nothing; a
  source can't be its own modulations' aux. Once a key is
  let go, a mod envelope that modulates something runs its release out even after the operators have faded: the
  voice keeps running its modulation alone (rendering no sound) until the release is over, so what it moves, an
  effect's knob for one, follows it down instead of jumping back, and only then reports its note's end to the host.
  - **Macros**: eight knobs (0–100 %) that hosts can automate, each with 16 slots like a source's. A macro's
    value times each slot's depth is added to its target for every voice and the effects alike, so a macro can
    also move effect params, which per-voice sources can't. Their names are part of the patch.
  - **Mod Wheel**: a macro of its own, its knob the Mod Wheel param (the wheel in the keyboard strip), with 16
    slots like a macro's. It moves nothing until something is assigned to it.
  - **Note**: a source that follows the pitch each voice plays, pitch bend, glide (and legato slides), note
    tuning and the global Pitch included. Like a macro it has 16 slots and no switch, but it
    runs per voice, like the LFOs and envelopes, and reaches the effects through the newest voice. What it
    modulates follows the pitch within the same block, without lagging behind it.
    Its **range** and **curve** shape what it puts out: the notes from **Low** to **High** (any MIDI note, C-1
    to G9, and on to D#10 +8, the pitch of 20 kHz, with their cents; High may lie below Low, which turns it
    round) run across a curve drawn like a
    shaper's, which sets what each puts out, from nothing to all of it; notes past either end put out what that
    end does. Over A3 to A5 with a straight line, it would put out nothing at A3, half at A4 and all at A5, and a
    curve drawn the other way round reverses that. The range and curve are part of the patch.
    As it starts, the range spans all of that, C-1 to D#10 +8, and the curve is drawn along each note's place on
    the cutoff knobs' curve, 20 Hz to 20 kHz: nothing up to the pitch at 20 Hz, D#0 +49, where the knobs start,
    then a straight line up to the top at 20 kHz (the dashed line in its editor). So it tracks exactly: a
    cutoff set to 0 and modulated 100 % by it sits right on the note's root frequency, and an A4 bent up two
    semitones takes it to 494 Hz. The filters' Key Track adds its own tracking on top, so leave it at 0, where it
    starts, for an exact match.
  - **Velocity**: a source that puts out the velocity each voice's note came with (in mono, the velocity of the key
    it plays now). Like the note source it has 16 slots and no switch, runs per voice, reaches the
    effects through the newest voice, and modulates from a note's first block on. Its **curve**, drawn like the
    note source's (up to 16 points, each segment bendable), runs across the velocities from 0 to 127 and sets what
    each puts out, from nothing to all of it. As it starts, the curve is a straight line, so it puts out the
    velocity as it came: half of it for a note at 64. Drawn the other way round, the softest notes move their
    targets the most. The curve is part of the patch.
  - An LFO runs in one of four clock modes, picked one at a time: **Retrigger** (the default), **Free**, **Anchor** or
    **One-Shot**. Retrigger restarts the LFO from its Phase with every note. Each of the other three is a switch
    hosts see; the editor turns the other two off as it picks one. Should a host turn on more than one, One-Shot
    wins, then Anchor.
  - **Free** makes an LFO run on without restarting at each note. All voices then follow one shared clock, which
    runs at the rate the newest voice hears (like the effects) and keeps going between notes.
  - **Anchor** ties an LFO to the host's transport: while it plays, the LFO stands where it would had it started at
    its Phase at the very start of the song and run ever since, so it lands on the same spot of its cycle at the same
    place in the song however playback started (a synced LFO counts in beats, so a quarter-note one is at its Phase
    on every beat; a free-running rate counts in seconds). All voices share it, as a free LFO's clock, whenever
    notes start. While the transport is stopped, or in a host that gives no position, it runs on from where it is,
    as a free one, and it lands back in place as playback starts. As its place comes from the song position times
    its rate, modulating the rate moves it along at once rather than speeding it up. A one-shot is never anchored.
    The browser demo has no transport, so there anchored LFOs run free; `fmvibe-uihost` plays its transport from the
    moment it starts.
  - **Phase** (0–100 %, 0 to begin with) is where in its cycle each note starts an LFO: at 25 % a sine starts at
    its peak, and 100 % is the cycle's start again. A free LFO keeps its shared clock and a random one its steps,
    so neither uses it; an anchored one stands at it at the song's start. A one-shot runs from there to the cycle's
    end, so at 50 % it plays only the second half.
  - **Sync** locks an LFO to the host tempo: its rate becomes a note value from 4/1 down to 1/64, each straight,
    dotted (D) or as a triplet (T). Without a tempo from the host, it assumes 120 BPM. The browser demo has no
    host: its page has a **Tempo** field under the editor instead (20–999 BPM, 120 to begin with, remembered by the
    browser), which every synced LFO, delay and sweep follows. A new LFO starts synced, in quarter notes. The note
    value takes modulation like a knob, from the editor's sources and the host's, but in steps: it moves from one
    note value to the next along the list (up for faster), never between them, and the dot on its knob stands on
    the one playing.
  - **One-Shot** runs an LFO through one cycle from each note and then holds it where the cycle ends (a one-shot
    is never free or anchored): for a sine, a triangle or a drawn shape that is where it started, while a saw holds
    at its top, a square low and a random one at the value it took. A drawn shape with a point at the cycle's end
    (100 %) holds at that point's level instead, so a ramp from 0 up to 1 there stays at 1.
  - **Unipolar** makes an LFO swing from 0 to 1 instead of −1 to 1, so it only pushes its targets one way, like an
    envelope. It is the same choice the LFO's right-click menu offers, and its display then has its zero line at the
    bottom.
  - **Random** takes a new value every cycle. **Chaos** sets how far it may move from the last one: up to a tenth
    of the range either way at 0 %, so it wanders, rising to anywhere at all at 100 % (the default), like sample
    and hold. **Smooth** glides to each new value instead of jumping: over that share of the cycle, easing in and
    out, then it holds (0 %, the default, steps).
  - **Custom** shapes are drawn from 2 to 64 points, starting as a triangle: up from the bottom to the top at the
    middle of the cycle and down again. Each segment between two points can be bent, from easing in
    to easing out, and the last segment wraps around to the first point, so the cycle loops without a jump. The
    points are saved with the patch (as hidden params, so hosts record edits for undo but don't list them).
- **Effects**, after the voices: everything the operators and filters send to the output runs through a chain of
  up to 16 effects. A new instance starts with an empty chain. Any effect can be added more than once, switched on
  and off, reordered and removed.
  - **Shaper**: picked from a list: Hard (a flat ceiling), Soft (tanh), Asymmetric (the lower half clips twice as
    hard, which adds even harmonics), Cubic (x − x³/3, scaled to reach full level), Gentle (x / (1 + |x|), which
    never quite gets there), Sine Fold and Fold (what goes past full level folds back, smoothly or in straight
    lines), or Custom, a curve you draw: up to 64 points from the quietest input to the loudest, each segment
    bendable, level past the ends. When its points move, the sound glides to the new curve over 20 ms, so dragging
    a point (the middle one above all, which sets the level of all that is quiet) doesn't crackle. What the curve
    puts out at the middle is taken out as it moves, so dragging it puts out no DC (made symmetric, the jump it
    makes there, as far as it shifts the average), and a DC blocker takes out the offset that the asymmetric and
    drawn curves, symmetric ones too, can leave. Drive
    (0–36 dB), Output and Mix; **Link** (on to begin with) brings the output down as the drive goes up, so
    turning the drive changes the tone more than the level: by as much as keeps a sine at −6 dBFS as loud (RMS) as it went in, dB
    for dB while the driven sine stays under the ceiling, then less and less as the clipping squares it off
    (about 8 dB at 12 dB of drive, 9 dB at the full 36). Louder sounds come out a little quieter, quieter ones a
    little louder. **Symmetric** (off to begin with), for a Custom curve, turns the curve's upper half over onto
    its lower one, so the input below 0 is shaped as the input above it, upside down: only the right half drawn
    counts (a curve that doesn't pass through the middle jumps there, and is 0 right at the middle, so silence
    stays silent). The switch shows only while the curve is
    Custom, and the other curves ignore it. The curves are anti-aliased (first-order antiderivative), so
    high drive stays clean without oversampling, though folding at the highest drives gets duller for it.
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
  - **3-Band Dynamics**: upward and downward compression in three bands, which Linkwitz-Riley crossovers (Low X,
    High X) split the mix into; at 0 % mix the bands just sum back up, so nothing combs. Each band is pulled down above its **Above** threshold by its **Down** ratio, as a compressor does, and
    lifted below its **Below** threshold by its **Up** ratio: the gap under the threshold shrinks by the ratio, so
    a level 6 dB under Below at 2:1 comes out 3 dB under it. **Range** (0–36 dB, 12 to begin with) caps the lift,
    so silence and the noise floor aren't raised without end; a ratio of 1:1 leaves its side alone. The ratios run
    from 1:1 to ∞:1, which holds the level at the threshold (a limiter above, and below a lift right up to Below, as
    far as Range lets it); their knobs turn them up by how much of a level's distance from the threshold they take
    away, so 2:1 sits in the middle and 4:1 three quarters of the way. Then comes the
    band's **Gain**. The detector follows each band's RMS level (**RMS**, on to begin with: a 10 ms average) or its
    peaks (held between them, falling back with a 20 ms time constant, so a wave's zero crossings don't read as
    silence); the gain follows it at the Attack pace while it falls and the Release pace while it rises. Knee
    (both thresholds' soft knees), Attack, Release, Range, Mix and Output are shared. To begin with, each band is
    pulled down 3:1 above −20 dB and lifted 2:1 below −40 dB. **Link** edits the bands together: while it
    is on, changing one band's threshold, ratio or gain in the editor (a knob, a line, a zone, a typed value)
    sets every band's to the same value, as one undo step. Each band always plays its own settings, so turning Link
    on changes nothing until the next edit, and a host's automation of one band's param moves that band alone. It
    replaces the 3-Band Comp,
    which the list of effects no longer offers: a project or preset saved with one before then loads it as a 3-Band Dynamics
    doing what it did, its crossovers, thresholds (as Above), ratios (as Down), gains, Attack, Release, Mix,
    Output and Link carried over (and the modulation aimed at them), Up at 1:1, a 6 dB knee and the detector on
    peaks; a linked one, whose bands all took the low band's threshold, ratio and gain, gives every band those.
    **OTT** in its card's right-click menu sets it as Ableton's OTT preset for its Multiband Dynamics has it, as
    one undo step: crossovers at 88.3 Hz and 2.5 kHz; Above at −33.8, −30.2 and −35.5 dB (low, mid, high), Down
    at 66.7:1, 66.7:1 and ∞:1; Below at −40.8, −41.8 and −40.8 dB, Up at 4.17:1; gains of +10.3, +5.7 and
    +10.3 dB; the mid band's 22.4 ms attack and 282 ms release for all three (OTT's are per band); hard knees,
    Range at 36 dB (OTT's lift has no limit), 100 % mix, 0 dB output, RMS on and Link off.
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
    sound stays at full level up to 50 %). The plugin keeps processing until the tail has died away, the
    pre-delay's wait included.
  - **Phaser**: 2–12 first-order all-passes (Stages) whose corner an LFO (Rate, 0–20 Hz; at 0 the sweep stands
    still) sweeps up to 2.5 octaves either way of Center (Depth), with Feedback (either sign) from the last stage
    into the first. **Sync** (off to begin with) makes the LFO go round once per note value of the host tempo
    instead, 4/1 down to 1/64 like an LFO's (a bar to begin with; 120 BPM without a tempo from the host). **Stereo** (off to begin with, under its display) runs the right side's LFO a quarter cycle
    ahead. Mix runs to 100 %, which is half wet: there the notches are deepest, and past it they would only get
    shallower again.
  - **Flanger**: a 1–15 ms Delay that an LFO (Rate, 0–20 Hz) sweeps by Depth, with Feedback of either sign (a
    negative one gives the hollower sound) and Mix, which runs to 100 %, half wet, like the phaser's (and starts
    there). **Sync** works like the phaser's, two bars a sweep to begin with, and so does **Stereo**, under its
    display, but on to begin with: the right side a quarter cycle ahead.
  - **Phase Disp** (phase disperser): 1–64 second-order all-passes in a row (Stages) at Freq, each as narrow as
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
  across the spectrum stays where it was without it. Effect knobs can be modulated like any other. Their levels and
  mixes ramp to a new setting sample by sample, however the host splits the block. As the effects are shared by all voices, the LFOs and envelopes
  reach them through the most recently played voice. Each of the 16 places in the chain has the parameters of
  every effect type, and hosts only list the ones of the effect it holds. Each effect gets a remote control page in
  chain order.
- **Global**: **Volume**, the level of the whole output after the effects (−60 to +12 dB, −12 to begin with, room
  for chords and stacked operators; at 0 dB a full-scale operator comes out at full scale. The voices go into the
  effects at a level of their own, 12 dB down, so the effects' drive and thresholds don't depend on it, and Volume
  gives those 12 dB back. A project or preset saved by FMVibe 0.1, whose Volume didn't, loads it 12 dB lower, so
  it sounds as loud as it did. Hosts modulate it globally, and LFOs and envelopes through the newest voice, as they
  do effect knobs), FM Amount, Pitch (±48 semitones, handy as an envelope target for drums), Pan, **Bend** Range for
  MIDI pitch bend (its knob over the bend wheel, in the keyboard strip), **Mod Wheel**, which moves what is assigned to it (nothing to begin with), and **Voices**, the
  polyphony limit (1–512, default 16).
- **MPE** (MIDI Polyphonic Expression), off to begin with: a setting of the instance, in the settings dialog (see
  Settings), saved with the project but not with presets, so browsing presets leaves it as it is. Hosts that
  send note expressions (Bitwig to CLAP plugins, Cubase to VST3 ones) need nothing: each note's tuning, volume,
  pan, pressure and brightness reach its own voice whether it is on or off. Over raw MIDI, with MPE on, channel 1 is the master channel, whose
  pitch bend (by Bend Range), controllers and channel pressure apply to every voice, and channels 2 to 16 each carry
  one note of their own: their pitch bend bends that note alone, by the **MPE bend range** (48 semitones to begin
  with, as the MPE spec has it; 12, 24, 48 or 96, beside the switch), and their channel pressure and
  CC 74 are that note's pressure and slide, which a MIDI source follows on those inputs. A note that starts on a
  member channel, or takes a mono voice over there, starts from what its channel last sent, and follows the master
  channel's pressure until its own channel sends some. With MPE off, every channel's bend, controllers and
  pressure apply to every voice, as without MPE. The plugin offers hosts the MPE MIDI dialect alongside CLAP's and
  plain MIDI. The browser demo takes no MIDI, so there MPE has nothing to act on.
- **Play modes**:
  - **Mono** plays one voice that moves from note to note. When you let go of the sounding key while others are
    held, it returns to the most recent one.
  - **Legato** (off by default): a note played while another key is down doesn't start its envelopes over.
    - In mono it only changes the pitch, and the envelopes and LFOs keep running. Without it, every note restarts
      them from their current level, so there's no click.
    - In poly the new note gets a voice of its own, which joins the voice of the newest key still down: its
      operators' envelopes and mod envelopes take up that voice's stage and level, and its LFOs that start with
      notes that voice's phase. A note played over a held one at its sustain starts at the sustain level rather than
      from the attack, and one played while the held note still swells swells along with it, so a chord played
      across held keys swells and wavers together instead of attacking note by note. (Over a held note whose
      envelope has decayed to a sustain of 0, the new note is silent, as in mono.) The new voice fades in over 5 ms,
      since its envelopes may start well above nothing. A key played with no other key down attacks as usual.
    - Patches saved before Legato acted in poly (state version 2 and earlier) load it off in poly, where it did
      nothing, so they play as they did; in mono they keep it.
  - **Glide** (0 = off, 1 ms–6 s) slides the pitch from the previous note, in mono and poly alike. By default it
    only glides between overlapping notes; **Always** makes it glide on every note. The slide is exponential and
    covers 99 % of the interval in the glide time. Glide can be modulated like any knob. Patches saved when it
    reached 20 s (state version 6 and earlier) load a longer glide as 6 s, and a modulation's depth on it larger by
    as much as its range shrank, so it swings Glide as far as it did.
- **Host modulation** (CLAP; VST3 has none, and VST3 hosts automate the parameters instead): all continuous
  parameters accept CLAP modulation globally, the macros' knobs and depths, the
  mod wheel, the note source's range and depths, velocity's depths and every modulation's Amount and curve among them; those
  the voices read also per
  voice (not Volume, the mod wheel, the effects, the macros, the note source or velocity), so Bitwig's modulators
  (including per-note ones) can drive them directly.
- **Notes**: up to the voice limit, with quietest/oldest stealing (a stolen voice fades out over 3 ms instead of
  clicking; a voice that hasn't played yet, and an operator still in its attack, count as loud, so a chord played
  into full voices takes the oldest rather than its own notes), and note expressions (tuning, volume, pan, and pressure and brightness for MIDI sources; see
  [CLAP and VST3](#clap-and-vst3) for the VST3 ones).
  Lowering the voice limit while more voices sound fades
  out the ones past it at once, as stealing does. Voices are stereo all the way through the filters.
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
the modulation panel or the keyboard strip makes it shorter (the plugin asks the host to resize the window).

- **ROUTING / MATRIX / FX / MODS** tabs, under the header, pick what the panels below them show: the routing graph
  or the matrix (two views of the same connections) next to the inspector, or across both the effects chain or
  every modulation. The
  choice is saved with the patch. They are folder tabs: the one shown is part of the panel under it, its outline
  curving into the panel's edge (ROUTING, in line with the panel's left edge, runs straight on down it), with the
  theme's accent along its top; the others are their names, a tab's shape
  showing faintly under the mouse. While the full-size shape, shaper curve or partial editor has the panels, no tab
  is shown, and clicking one closes the editor (a widened LFO card's shape stays in its card).
- **Routing panel**: the graph shows only what is switched on, the matrix everything, greyed out where it is off.
  The inspector's tabs switch nodes on and off (see below); so does a node's right-click menu in the graph.
  - **Routing** (graph): circles are operators (with their wave and ratio), rounded boxes the filters (with their
    type) and squarer boxes the shapers (with their curve). A colored outline means the node is heard, as its routes are set, and a
    halo glows around it while it sounds: an operator's as brightly as its envelope and the routes taking it to the
    output let it through, where modulation takes them now, so a route an LFO opens from 0 % lights it up as it
    opens. The nodes that are off wait in the graph's top left as dashed ghosts, a circle with an
    operator's letter or a box with a filter's or shaper's name; a click switches one on, back where it was in the
    graph, and shows it in the inspector. However it is switched on (its ghost, its tab, the matrix), a node whose
    place another node shown now covers comes back clear of it: pushed out of the way, or, with no room there, at
    the free place nearest it (in the same undo step as the switch).
    - Drag from a node's **edge** (a band along its outline, which glows in the node's color under the mouse)
      onto an operator to modulate it, back onto itself for feedback, onto a filter or shaper to send it there, or
      onto the **OUT** bus; from further inside, a drag moves the node, and a click on the edge only picks the node.
      Filters' and shapers' edges work the same way: onto an operator to modulate it, onto a filter or a shaper,
      back onto itself for feedback (starting at 25 %, its loop drawn on its top edge), or onto the bus. Every node
      can feed every other one, and itself. Operators, filters and
      shapers go out alike: each has its line to OUT while it is routed there, whether anything goes into it or not.
      A node that already goes out is moved through a filter or shaper it is connected to rather than doubled, its
      output's modulation going along onto the send, and that goes out in its place unless its sound gets there already (through filter 2, say); so does a filter or
      shaper a node's arrow to OUT is moved onto. Wires to or from a filter are
      purple, those to or from a shaper red, like the shaper effect.
    - A node's line to OUT ends in an arrowhead on the bus, like a wire's on the node it feeds, with its
      percentage label halfway down (beside the line, on its left, when the node sits too close to the bus for
      it). Drag an arrowhead onto another node to move the route there, or onto the bus to send its node out
      instead; the line to OUT's onto a node the same way. The route keeps its amount and its modulation, and
      follows the mouse until it is let go; it can't go where its node has a route already, or where a node's edge
      couldn't take it. Let go of off the nodes and the bus (where it turns faint), the route goes, with its
      modulation, as its right-click menu's Disconnect does. The feedback loop above an operator or a shaper has no
      arrowhead.
    - Drag a wire's percentage label up or down to set the depth; double-click it to remove the wire, and
      Ctrl+Click it (or pick Edit value… from its right-click menu) to type a depth. A wire, or
      a node's line to OUT, turned down to 0 % stays, so modulation can still be dropped on it; only removing it
      (double-click, or Disconnect in its right-click menu) takes it away, together with its modulation. Dropping
      an LFO or envelope on a matrix cell or an Out knob shows that route's wire too, even at 0 %.
    - A wire between two nodes bows to its left as it starts, so two running opposite ways between the same nodes
      don't overlap. To keep wires apart where they would, drag one sideways anywhere along it (it lights up under
      the mouse, apart from its label and arrowhead): its middle follows the mouse, out to either side or straight,
      its label and arrowhead going along. A wire that would spill out of the routing panel bows only as far as
      keeps it and its label inside, whether it is bent or its nodes are moved toward the edge, and bows as it was set again
      once there is room. A double-click on it, or Reset Bend in its right-click menu, brings back
      the bow it started with. How each wire bends is saved with the patch, like the node places, and each drag is
      an undo step; a wire that is removed or moved to another node starts bowing as a new one does.
    - Drag nodes to rearrange the graph (saved with the patch). Click one to edit it. A node dragged onto another
      stops short of it, held 10 px off its outline and sliding round it, and goes on once the mouse is past.
  - **Matrix** (grid): each row is a source (an operator, filter or shaper), and its cells say how much of it goes
    to each column: the operators, F1 and F2, S1 and S2, and OUT. It always lists all ten nodes and fills the
    panel's height, as far from its top as from its bottom; every cell is a route; the row and
    column of a node that is off are greyed out, its routes kept as they are. The diagonal is each node's feedback,
    outlined in its color; toggled on, it starts at 25 %. Drag cells
    vertically; double-click toggles a cell on or off; Ctrl+Click types an amount. Dragging a cell, or toggling it
    on, switches on the nodes it joins. A row's or column's name works like the node's tab in the
    inspector: clicking it shows that node there, switching it on if it is off, and the names of the node shown
    sit on a plate, like its tab, where a click switches it off, or on again.

  - **FX**: takes the place of both the routing panel and the inspector. It shows a card per effect, left to right
    in processing order, each in its type's color (shaper red, delay blue, DJ EQ amber, Param EQ orange, filter
    purple, compressor green, 3-band dynamics teal, reverb indigo, phaser pink, flanger lime, frequency shifter
    cyan, phase disperser slate, widener fuchsia) and
    with its icon, which switches it on and off, an × to remove it and a live display:
    - the shaper's transfer curve; a Custom curve shows its own points and bends instead, to drag right there
      (double-click adds or deletes a point, Ctrl snaps to the grid), with what the drive, output and mix make of
      it faintly behind and the point under the mouse in numbers; its edit button (a pencil) opens the big editor;
    - the delay's echoes on a timeline, left above the line and right below it;
    - the EQs' and the filter's responses; behind the Param EQ's curve, what it puts out, measured (a 2048-point
      FFT in 96 bands from 20 Hz to 20 kHz, 0 dB at the top, falling away at 55 dB a second), which it only
      measures while the editor is open; the Param EQ's and the filter's sit on a grid, dashed every 12 dB with
      0 dB solid, and dashed at 100 Hz, 1 kHz and 10 kHz;
    - the compressor's curve (input level across, output up) with a gain reduction meter;
    - the 3-band dynamics' levels: a bar a band, the high band on top, from −60 dB at the left to 0 dB at the
      right, with the band's name at its left and its gain at its right. Down the bar's middle runs a meter half
      its height: in the card's color up to what comes out, then the gain, amber where the band takes some off
      (up to what went in) and in the accent where it adds some; a white tick across the bar marks what goes in.
      Left of the Below threshold, where the band lifts, and right of the Above one, where it pulls down, the bar
      is faintly tinted (more under the mouse), and thin lines
      (the text color, at half strength) show the ratio: one for every 6 dB that goes in past the threshold, drawn
      where the ratio takes it (Range and the knee left out), seven at most, as far as the bar's end. At 1:1 they
      stand 6 dB apart; they close up evenly toward the threshold as the ratio turns up, and from about 15:1 on stay
      0.4 dB apart. They move with their threshold: toward the bar's end it pushes them out past it, and moving back
      brings more in. A band's row picks the band its first row of knobs (Below, Up, Above, Down and Gain)
      sets, the band's name on a plate; the line under those knobs is named after it (ALL BANDS while linked),
      and the shared knobs come in two rows under it, Low X, High X, Attack and Release above Knee, Range, Mix and
      Output. Each bar's threshold lines drag (lit under the mouse): across, a line moves its threshold (Above
      stops at Below, and Below at Above), showing it where the gain was; Alt-dragged up and down, or turned with
      the wheel, it sets that side's ratio; a double-click puts its threshold back. The zones beyond the lines
      drag up and down for their side's ratio, the way the band moves the level: the upward one (left of Below)
      up for more, the downward one (right of Above) down for more (the wheel turns them too, and a double-click
      puts them back), showing it where the gain was. Pressing a line or a zone on another band picks that band. Linked, dragging any
      band's line or zone moves every band's the same way, and every band's readout shows the value. RMS and
      Link sit in the card's title row. The
      card is one and a half places wide, like the Param EQ's. The Below, Up, Above and Down knobs, and Range,
      say in a tooltip which way they work ("Downward threshold", "Upward ratio").
    - the reverb's tail: after the pre-delay it builds up (slower in a bigger space, with its early reflections
      spread wider) and falls to nothing at the decay time, its texture sparser the bigger the space; a room's
      early reflections stand out, a plate is dense from the start, and a shimmer's shifted tail sparkles above it
      (below it for a shift down), each step of the interval later, further out and fainter, the less it shifts
      the fainter, with the interval over the display. Its algorithm comes from a list under the card's title,
      like the shaper's curves; a shimmer's card has two rows of four knobs, its Pitch and Shimmer among them;
    - the phaser's and flanger's responses faintly at both ends of their sweep, and bright where their LFO is,
      moving while the audio runs (a stereo one's right side as a second, thinner curve);
    - the phase disperser's delay across frequency, peaking at Freq, with what it puts out, measured, behind it
      like the Param EQ's;
    - the frequency shifter's move: a 220 Hz tone's partials, and where it takes them on each side, under the
      shift in Hz and the range it is a share of;
    - the widener's stereo field: left to right across, frequency up (20 Hz to 20 kHz), the dry sound a line
      down the middle and the sides it adds spreading out above the high-pass (a dashed line), as far as Width
      and Mix take them, wavering with the sweep while the audio runs; what Width leaves in the middle thickens
      the line. Above it, the high-pass and the taps' delays.

    The delay's switches under its display are icons across the card, named on hover: a metronome for Sync, two
    chain links for Link (the right side follows the left) and a ball between two paddles for Ping-Pong. The
    shaper's Link (which ties its Output knob to the drive) is the same chain, and so is the 3-band dynamics' (in
    its title row), centered on the card under its display; for a Custom curve the shaper's Symmetric shows left of it, a curve
    rising to level off, its left half the right one turned over, the two centered together. The shaper's curve's list takes the whole row above the display. Every effect with a Sync
    has that metronome: the phaser and flanger at the left under their displays (in one pill with their STEREO),
    over their Rate knob, which, synced, picks a note value and shows it as its label, as the delay's time knobs
    do. The filter's slope is a list under its display, like a voice filter's (see the inspector). The filter's
    knobs come in two rows, Cutoff, Reso and Key Track above Drive and Mix, and so do the
    frequency shifter's, Range, Shift and Spread above Feedback and Mix.

    Each effect has an icon, at the left of its card's title and in the list of effects. On the card, the icon
    is the effect's switch: a click turns it off (the icon turns grey with the card, under the mouse too, and the
    effect costs nothing) or on again, in its color; dragged, it moves the card like the title. **+ EFFECT** (a drawn plus before the word) at the panel's top left
    opens a list of the effect types (beside it, how many of the 16
    places are taken, as in 3/16), and so does a dashed card with a plus after the last effect, over itself; the
    one picked goes at the end of the chain, which scrolls to show it, and so does a copy. The arrow between two
    cards turns into a plus under the mouse: a click opens the list over the cards there, and the one picked goes
    between those two. The dashed card takes a
    card place at the chain's end while there is room for another effect. Six cards fit across, the Param EQ's
    taking one and a half places for a wider spectrum; with more,
    a scrollbar under them (drag its thumb or click its track; drawn thinner while the mouse is away from it) or the
    mouse wheel over the panel scrolls the chain smoothly, cutting off the cards at the ends, and so does a drag with
    the middle button held (see Everywhere). Drag a card by its
    title to move it along the chain (held at either end of the row, it scrolls the chain along that way),
    and right-click the title (or the icon) to duplicate the effect (with its settings), reset its settings,
    set a 3-band dynamics as OTT, or remove it. The chevron left of a card's × folds it to a narrow strip, as wide as the icon's place at the
    title's left, so the icon stays where it was: the icon, which still switches the effect and drags the card,
    its ×, then its name down the strip, which unfolds it. The cards after it close up; folds are saved with the
    patch but not undone. On the Param EQ's display,
    drag a band's numbered handle for frequency and gain, Alt-drag it up or down (it stays where it is) or scroll
    on it for Q, and double-click it to flatten it; a cut's handle sets its resonance instead of a gain, and a
    double-click takes the resonance away. Each band's
    row of knobs starts with its shape, an icon in the band's color with its name under it, which opens the list
    of shapes.
  - **MODS**: takes the place of both too: every modulation as a row of a table, by source (the LFOs and envelopes
    in the order their cards show, then the macros, the note source, the mod wheel and velocity), the first of a
    source's rows naming it with its handle's look and the others a line in its color down the source's column.
    Modulations at 0 % are listed too, and after them the empty ones (see below). Over the rows, the columns' names,
    shown with no modulations too (with a hint under the button that adds one), and at the right of their row the
    count (how many are bypassed, at 0 % and empty with it, as much of that as fits), then the **filter**'s
    magnifier. A click on it opens the filter, a text field over the count that takes the keyboard: what is typed
    narrows the rows to the modulations whose source's and target's names hold every word of it, whatever the case
    (the empty modulations stay, having yet to get theirs), and it says at its right how many it shows of all.
    Its × empties it, Escape empties it and then lets the keyboard go, as Enter and a click elsewhere do; while it
    holds something it stays open. A source card's MODS button empties it, so the source's rows show. A row has,
    from its left:
    - the bypass dot, which switches the modulation off and on again, whatever its source;
    - the **source** and the **target**, and between them, after the source's name on the first of its rows, the
      source's **polarity** switch: a wave about a middle line while the source is bipolar and humps on a baseline
      while it is unipolar (the LFO's UNIPOLAR icon), flipping it between the two for all of its modulations, as its
      right-click menu does. A click on the source or the target opens a cascading menu, the next level opening beside
      an entry under the mouse. The source's lists the sources by category (LFOs, Envelopes, Note & MIDI for the Random and MIDI
      sources, Macros, and Keyboard:
      Note, Velocity and Mod Wheel); picking one moves the modulation there, into a free slot, with its depth,
      curve, Amount, aux and bypass, as one undo step; it then swings as its new source does. What modulates its depth
      follows it to its new slot, or, moved to a macro, the note source, the mod wheel or velocity (whose depths
      can't be modulated), goes. A source that modulates the
      target already, or has no slot free, is listed faintly, saying why, and can't be picked. The target's lists
      every target by category: Global (but the mod wheel, which is played rather than set); Operators, each with
      its params in two columns, under PITCH, LEVEL, PHASE and UNISON, and beside them ENVELOPE and NOISE;
      the Matrix, by the node a route comes from (into each other node it can feed, out, and its feedback last); Filters; Shapers; Effects,
      only those in the chain, in its order, each with the knobs of its type; and Modulation, each LFO's and
      envelope's own knobs and the depths of its modulations. Picking one moves the modulation there, keeping the
      rest. What the row has now is checked, and the categories it is in have a dot; a click elsewhere closes the
      menu, and so does a second click on what opened it. Atop the target's menu, a **search** takes the keyboard
      while the menu is open: as soon as something is typed, the menu's first level lists the targets whose path
      (their categories, then their name, as in "Operators › B › Detune") holds every word of it, flat, each with its
      path dimmed before its name, up to 16 of them, then how many more there are. The first one found that can be
      picked is lit; Up and Down light another, Enter or a click picks one. Escape, or the search's ×, empties it,
      and Escape with nothing typed closes the menu. Both fields edit like the preset browser's search (see the preset bar);
    - the **depth**, a bar like a source row's, with its value (double-click or Ctrl+Click types one);
    - the **curve**, drawn along the source's swing (from the middle out for a bipolar source), faint while it is
      straight and in the source's color once it is bent; dragged up or down it bends (100 px from straight to
      either end), and a double-click straightens it. Its tooltip gives its value;
    - the **Amount**, a bar from none at its left to all of it at its right, dragged or clicked, with its value;
    - the **aux**: a chip in its color with its name and an × that takes it away, or a dashed **+ Aux**; a click on
      either opens the menu of sources, None first, the row's own source faint;
    - the × that removes the modulation. Removing a modulation, here or anywhere else, removes whatever modulates
      its depth with it, in the same undo step.

    The right-click menu of any of a row's cells is its modulation's (Edit depth on the depth, Bypass, Remove).

    **+ Add modulation**, a dashed button after the last row, adds an **empty modulation** there and opens its
    source's menu. An empty modulation is a row with no source or no target yet ("Source…" and "Target…" in their
    places, a hollow dot, and only its ×): it modulates nothing. Picking its source and its target from their menus,
    in either order, makes it a modulation of that source, as its handle dropped on the target makes one (at +30 %),
    which takes its place among the source's rows; until then its source's menu offers any source with a slot free,
    and its target's every target. An empty modulation whose source already has rows goes right after them, as
    soon as it has that source (added from it, or once it is picked), and the list jumps there, showing as many of
    the source's rows above it as fit; so does the modulation it becomes. Each pick is an undo step, and empty modulations are saved with the patch (up to
    32 of them). With more rows than fit (about ten), a bar at the right (drag its thumb or click its track) or the
    mouse wheel over the rows (not on a bar, which the wheel turns) scrolls them smoothly, a row a notch; a drag
    with the middle button held drags them up and down.
  - **Partial editor**: picking Custom from an operator's **…** list opens its partials over the routing panel:
    the wave they make, one cycle, then a bar per partial for its level (in dB, from −60 just above the bottom
    to 0 at the top, marked every 12; the bottom itself is none) and, in another color, one for its phase (a full
    cycle at the top, faint while the partial is silent). Drag across the bars to draw them, double-click
    a bar to take it to nothing; hovering shows a partial's numbers. **16 / 32 / 64**, after the title, pick how
    many partials the wave has, and so how many bars there are (numbered each, or of 64 the first and every
    fourth); the ones a lower count hides keep what was drawn. The reset button (a circular arrow) goes back to a
    sine, leaving the count, the done button (a check mark), or a tab, closes it, and Custom from the list or the
    edit button (a pencil) on the operator's summary line opens it again. A stroke is one undo step.
  - **Shape editor**: picking an LFO's Custom shape, a click anywhere on a Custom LFO's display (which shows an
    edit button, a pencil, in its corner), or the expand button in its title row, left of the fold button, widens its card to two card places, the cards after it moving along and the row scrolling to show it
    whole, to edit the shape right there while the panels above stay in use: the inspector's knobs, the routes and
    the other views can be turned and switched as the shape changes. The widened card keeps the LFO's own controls where a normal card has them,
    with the shape tools and drawing modes where its display was (see below), and the shape's plot beside them,
    264 px across; under the plot, the point under the mouse in numbers (its number, its place in the cycle and its
    level) and the grid's steppers, and over it, as its tooltip, how to edit it. Its title row has, before the
    fold, power and remove buttons, the reset button (a circular arrow), which puts back the shape it started with,
    the collapse button (two arrows pointing in), which takes the card back to its size, and right of it the
    full-size button (two arrows pointing apart); another shape, folding the card and removing it take it back to
    its size too. Every edit applies as it is
    made, so there is no done button. One card is widened at a time: widening another takes the first one back.
    Collapsing the modulation panel leaves the card as it was, widened or not, for when the panel opens again; it
    isn't saved with the patch. The full-size button opens the shape over the routing panel and the inspector,
    which it hides while it is open, the card staying widened under it with its own editor greyed out (it takes no
    input there meanwhile); the done button (a check mark, top right) closes it, as a view tab does, and the card's
    editor is back in use.
    The **shape tools**, a row of icons (named on hover), each change the whole shape in one undo step, a quick
    second click being another: **Invert** flips it upside down; **Reverse** plays it back to front, the first point
    staying first and each segment bent the other way; **Shift left** and **Shift right** move it a column of the
    grid earlier or later, round the cycle (where the new start falls, a point is put on the curve if there is none
    there, splitting its segment without changing its course); **Copy** keeps the shape, and **Paste** puts it into
    another LFO (faint until a shape is copied; for now LFO shapes only). Under them, **Points | Steps** picks what a
    click on the shape does, in the card and in every big editor below, which has them too (left of its GRID, or in
    a shaper effect's narrower editor in its title row, left of the reset button): with Points, as below; with
    Steps, a click draws a step, as Shift+click does with Points, and Shift+click works with points. **Snap** snaps
    points and steps to the grid without Ctrl, which then lets them go (a shaper's curve drawn right in its card or on
    its page, which show no grid, snaps with Ctrl only). Both belong to the editor, like the grid, and go for every
    shape and curve. How to edit a shape, as they have it, shows in each editor's title row (a shaper effect's under
    its plot) and as a widened card's plot's tooltip.
    Drag points to move them; the point under the mouse is shown in numbers, its level as the LFO puts it out (0 to 1 for a
    unipolar one). A point stays between its neighbors, but can reach a neighbor's time: points at one time make a
    line straight up or down (a spike, say), and at that moment the LFO takes the last of them in order, the one
    the shape goes on from. Double-click empty space to add a point and a point to delete it (or right-click it).
    Shift+click (over a point or a bend dot too) draws a step, as in a step sequencer: a level line at the
    click's height from the grid line left of it to the one right of it, with a point at each end (Ctrl snaps its
    height to the grid too). The points between those lines go; a step beside another starts where that one ends,
    at the same place, so the shape jumps straight up or down there, and a step in the first column takes the
    first point. Held, the step follows the mouse: up and down to another height, and across to another column,
    the one it left going back to how it was, until the button is let go (one undo step). Shift+click a step
    again to redraw it at another height. An LFO's shape and a shaper's curve
    hold 64 points, so every column can hold a step even on the finest grid, 32 across; the note source's curve
    and velocity's hold 16. The small dots halfway along each segment bend it (a jump has none): drag them up or down,
    double-click to straighten. **GRID**, under the
    editor, sets how many lines run across (2–32, thirds included) and up and down (2–16), with its − and +
    buttons or the mouse wheel; hold Ctrl to snap points to them. The grid belongs to the editor: it is saved with
    the project, and presets leave it alone (so do Points | Steps and Snap). The first point always sits at the start of the cycle. A shaper
    effect's Custom curve opens the same editor from the edit button (a pencil) on its card's display, in a panel in the middle of the
    effects' (the inspector stays hidden), its plot closer to square, with input across and output up; its first
    and last points stay at the ends. While the shaper is symmetric, only the curve's right half is edited, in
    the editor and in the card alike: the left half is drawn as its mirror (behind a veil in the editor) with faint
    rings for the points it mirrors, the points left of the middle are hidden, and a click there counts as one on
    its mirror image. Switching Symmetric on in the editor gives the curve a point in the middle if it has none,
    which then stays there: it only moves up and down, and can't be deleted (a step drawn across it keeps it, at the step's height). The note source's curve opens over the routing panel from its display in the
    keyboard strip (a second click closes it): the notes from Low to High across, the Cs among them named under
    the plot, and what the source puts out up, from nothing at the bottom to all of it at the top, with a dot
    where the newest voice is while notes play. A dashed line shows, for the range set, what would take a cutoff
    at 0 by 100 % onto the note. **Low** and **High** are knobs side by side in its title row, dragged in whole
    notes (or typed, as `C4`, `F#2 -20` or a MIDI note number); its ends stay at the ends too, and the reset button
    brings back every MIDI note and the curve along the dashed line. Velocity's curve opens over the routing panel
    the same way, from its display (see Velocity, under the keyboard strip).
- **Header**: everything between the name and the voice count is centered between them. First the output scope:
  the output after Volume (its two sides' mean) over two periods of the newest note's pitch (kept once the note has
  ended, as the effects' key tracking keeps it; a high note's span holds a whole multiple of two periods, at least half
  a millisecond). Its spans follow one another at that pitch, so a sound that repeats with the note stands still
  whatever its wave, and a new note starts them over from where it starts; silence shows as a flat line. Its points
  fall between samples, and are taken band-limited from the 16 samples around them, so a wave dense with high
  partials looks the same wherever the samples fall in it. The trace is
  drawn to a level that follows its peaks, up at once and down to a third in about 300 ms, so a level moving from
  span to span doesn't make it jump; that level sits at the display's edges, lifted by no more than 24 dB, so the
  shape shows at any volume while a fading tail shrinks away. The audio thread captures only while the editor is open. Then the output meter, left of Volume:
  a bar for each side showing its peak, from −48 dB at the bottom,
  in the theme's color up to full scale and red past it, falling 24 dB a second (also once the plugin stops
  processing); the strip above full scale lights up red for a second and a half after the output went over. Then
  the global knobs; the play cluster between two dividers (Poly/Mono over Legato, which works in both, then
  a Glide knob like the global ones, its Always switch beside its label, each switch explaining itself on hover);
  Voices (it drags on a log scale, so 4 and 512 are both easy to reach); and how many voices are playing.
- **Undo and redo**: the curved arrows left of the preset buttons step back and forward through the patch's
  edits, up to 100 steps; hovering one says what it would undo. A drag, a run of wheel notches on one knob, a typed
  value, adding or removing an effect or source (with its modulation), moving nodes or cards and bending a wire are
  each one step. The history lives in the plugin, so it outlasts closing the editor, and goes when the host loads a
  saved state, at Init and when a preset loads: undo never brings back the patch a preset replaced. **Ctrl+Z** undoes, and **Ctrl+Y** or **Ctrl+Shift+Z** redoes, while the mouse is over
  the editor and its window is the active one: a keyboard hook catches them before the host does, so the host's
  own shortcuts keep working everywhere else.
- **Inspector** (right): tabs A–F, F1 and F2, and S1 and S2 pick what it shows; tabs of switched-off nodes are
  dimmed, and clicking one switches its operator, filter or shaper on; the names of switched-on ones take a little
  of their node's color. A dot in a tab's top right corner says the
  node's own sound goes out: its Out is up and it is switched on (a filter's or shaper's only while something goes
  into it). Clicking the
  selected tab switches it off, or on again (its
  tooltip says which). Under the tabs, a summary line sums the node up in short: its letter, then an operator's
  wave, ratio, unison copies (×4) and where it goes ("A · Saw · 1.20 · ×4 · to F1"), or a filter's type and slope,
  or a shaper's curve, and what feeds them, where they go and the operators they modulate. The power switch next
  to it does the same as clicking the selected tab. While the node is off, its page is dimmed below the summary
  line; its knobs still work.
  - Operators: wave buttons for the common waves, then **…**, which lists Pulse, Half Sine, Abs Sine, Alt Sine,
    Organ, Vowel and Custom (picked,
    one of these shows on that button); the Ratio readout, then pitch, detune, phase and output knobs. The Ratio
    readout takes the first two knobs' places: four digits of a 7-segment display (88.88, the tens dark below 10),
    each its own handle. A digit dragged up or down steps by its own place (tens 10, ones 1, tenths 0.1, hundredths
    0.01), a step every 6 px (24 with Shift), carrying into the next digit and held to 0.50 to 64.00; the wheel steps
    the digit under the mouse, and a double-click rounds to it (on the ones, 3.47 becomes 3.00). The digit under the
    mouse, or being dragged, is lit behind. Ctrl+Click or the menu's Edit value types a value, which can be a
    fraction (7/2) or start with ×. Over the digits a track runs from 0.5 to 64 in a straight line: a tick at the ratio (at its right end at 64),
    each modulation's reach in its source's color (wider while its depth handle is hovered, with an arrow after the
    end where it goes on past 64), and a dot where
    modulation takes the newest voice; the depth handles sit along the box's bottom edge, clear of it, and drag an octave of depth every 60 px (50 times finer with Shift), and a source's handle
    dropped anywhere on the box modulates it. A keyboard on the summary
    line, left of the power switch, switches fixed pitch: faint while the operator follows the keys, crossed out
    and lit while it is fixed. Switched on, Freq and Mult are set to where the ratio put an A4 (the smallest Mult
    that keeps Freq in its range), in the same undo step, so an A4 sounds as before; Freq and Mult knobs take the
    Ratio readout's place, Pitch and Detune are dimmed and take no drags, wheel or typing (nor do their depth
    handles), and the summary line and the operator's circle in the graph show the frequency in Hz instead of the
    ratio. Switched off, the ratio, Pitch and Detune count again as they were. While the wave is Custom,
    an edit button (a pencil, the same as a Custom LFO's and a drawn shaper curve's), left of the keyboard, opens
    the operator's partial editor (lit while it is open; a second click closes it). Under the knobs, tabs pick what
    the lower half of the page shows, the same for every operator and saved with the project (a preset leaves it
    as it is); a hidden tab whose knobs a source modulates has a dot in its corner, and holding a dragged source's
    handle over a tab opens it, so its knobs can take the drop:
    - **ENVELOPE** (to begin with): the operator's envelope, and its knobs. While notes play, a dashed line across
      it shows the newest voice's envelope level, with a dot where it meets the curve, as a mod envelope's card
      does.
    - **UNISON**: the unison knobs and Rnd Phase, and a display of where the copies sit (left/right by spread,
      up/down by detune).
    - **SCOPE**: the operator's output in the newest voice as it plays, with its unison copies together and
      everything that moves it (the FM it gets, its envelope, the modulation on its level and pitch), before it
      reaches the filters, shapers or the output. It is caught at the operator's own phase, so a span starts where a
      cycle does and stands still even under heavy FM. The buttons in its corner show 1, 2 or 4 cycles across
      (2 to begin with): real cycles, so a modulator at a ratio of 0.5 or detuned copies make them differ. The trace
      is drawn at its level, between dashed lines at full scale, its level in dB in the top left corner: following
      its peaks as the header's output scope does, up at once and down slowly, so it doesn't flicker. A slow
      operator, whose cycles take longer than a fifth of a second, is drawn as it goes, up to a line, what is left
      of the last span faint after it. With no note playing, the wave's plain shape shows faintly. The audio thread
      captures only while the tab is shown.

    A noise operator shows Slope and Spread instead of the pitch knobs, and no Phase; its first tab is
    **SPECTRUM**, its spectrum against white, pink and brown, and it has no scope (picked, it shows its envelope).
    Its circle in the graph is labeled with its color instead of a ratio. The envelope is a curve with draggable
    points: the peak (attack), the end of the hold just after it, the sustain level (decay and sustain) and the end
    (release). The release starts at the sustain level's point: the sustain has no length of its own, lasting as
    long as the key is held, so the display gives it none. The small dot halfway along the attack, decay and release bends that segment (drag up or down,
    double-click to straighten it, to 0 %). A hovered or dragged handle's values show at the display's top right,
    over the curve. The display always zooms to fit the whole envelope across its width, so the handles of
    short segments stay apart; the zoom holds still while a handle is dragged, so it stays under the mouse, until
    the envelope would run past the display's end: then it zooms out to keep all of it in, the handle at the end,
    and dragging on lengthens the segment further. The same goes for the mod envelopes' displays. In the graph, an operator with unison is drawn as a stack of rings.
  - Shapers: the summary (its curve, what feeds it, the filters it feeds, whether it goes out and the operators it
    modulates, as a filter's has), its curve's list with the link switch beside it, and for a Custom curve the
    symmetric one between them (as on the shaper effect's card), the Drive, Output, Mix and To Out knobs,
    and its transfer curve; a Custom curve shows its points and bends there to drag, like a shaper effect's card, and its
    edit button (a pencil) opens the big curve editor over the routing panel, the shaper's page staying beside it. Underneath
    are the send knobs of the operators and filters that feed it.
  - Filters: the summary (its type and slope, then what feeds it, what it feeds and whether it goes out, and the
    operators it modulates, as a shaper's has), type buttons, each a small picture of its response (named on
    hover), and the slope's list at the end
    of their row, 6, 12, 18 or 24 dB (a band-pass's or notch's shows 6 and 18 faintly and can't pick them, and shows
    what plays; for a comb it is faint and doesn't open), cutoff, resonance, key tracking, drive and output, and a live response curve whose
    handle drags cutoff (sideways) and resonance (up/down). With its cutoff or resonance modulated, faint curves
    show how far the modulation takes it either way, and while notes play the curve follows where the newest
    voice is (the handle stays at the set values, the cutoff a C4 has). Key tracking moves all of it with that
    voice's note, an octave per octave at 100 %. The filter effect's display does the same, its key tracking
    staying where the last note left it, as the effect's does. It sits on the filter effect's grid. Behind the curve,
    like the filter effect's, is what the filter puts out, all voices together, measured before its output level
    (the Param EQ's analyzer: a 2048-point FFT in 96 bands, 0 dB at the top), while the editor is open; a filter
    that is off shows none. Underneath are the send knobs of everything that feeds the filter.
- **Modulation**: the panel's title row has the **MODULATION** toggle, which collapses the panel to that row and
  opens it again (the editor gets shorter; saved with the project, but not part of presets), then **+ LFO**,
  **+ ENV**, **+ RANDOM** and **+ MIDI** (a drawn plus, like + EFFECT's) to add a source (which opens a collapsed
  panel). A Random card says what it does where the others have knobs. A MIDI card has its input's list (CC,
  Pressure or Slide, where an LFO's shape is; its tooltip says what the input follows), and right of it its CC knob
  (while its input is a controller) and Slew; under them, both cards'
  display is a bar of what the newest voice gets now (from the middle while the source is bipolar), with its value.
  Below, the sources' cards show four at a time, and after the last one, while there is room for more, a dashed
  card with a plus, **ADD SOURCE**, as tall as the cards, opens the list of source types over itself (LFO, Envelope,
  Random and MIDI, each with a small picture of what it puts out); a pick adds one, and a click elsewhere closes
  the list. With more than fit, the scrollbar under them (thinner while the mouse is away from it, like every
  scrollbar) or the mouse wheel over the panel scrolls them smoothly, and a drag with the middle button held drags
  them along. Sources are numbered by type in the order they show ("LFO 2", "Env 1", "Random 1", "MIDI 1").
  - Drag a card by its title (anywhere in the row but its handle and buttons) to move it along; held at either end of the row, it scrolls the row along that way.
    The order is saved with the patch. Right-click the title (or the handle) to duplicate the source (with its
    settings and targets), clear its targets, make it **Unipolar** or **Bipolar** (the one in force checked) or
    remove it; the × removes it too, together with its modulation and whatever modulates its knobs. A bipolar
    envelope's display has a dashed line at
    its middle level and ± in its corner. While notes play, an envelope's display has a dashed line across at the
    newest voice's level, and a dot on the curve as far along the stage the envelope is in as it has come: along
    the attack, decay or release, across the hold, and at the sustain level's point through the sustain. While the
    envelope follows its knobs, the dot sits where the line meets the curve. Where it doesn't (a note let go
    during the decay releases from above the sustain; modulation moves the sustain), the dot keeps to the curve
    and the line shows the level.
  - The power switch turns a source on and off; one that is off has its card dimmed, its name grey (as an effect's is), and its handle and its rings
    on knobs faint (the handle can still be dragged onto targets).
  - The chevron left of the power switch folds the card to a narrow strip, as on the effects: the handle, a grip
    to drag it by, the switch and ×, then the name, which unfolds it.
  - Each source has a colored handle in its card's title row. Drag it onto any knob, wire label
    or matrix cell; a filter's cutoff is the classic target. Held near the left or right end of the modulation
    panel or the effects chain, it scrolls those cards that way, faster nearer the edge, so a knob on a card out of
    view can be reached. Hovering the handle draws a dashed line to each of its targets on screen, ending at the
    small dot it has there (see below), with a ring round the dot; while the source has no targets, it shows a
    "Drag to assign" tooltip instead, as the macros', the note source's, the mod wheel's and velocity's handles do.
    A source that is another modulation's aux (see below) also gets a dotted line, in its color and bowed the
    other way, to that modulation's depth handle on screen, which is ringed with dots.
  - A card doesn't list its modulations, and ends with a button under its display: it counts them, with **MODS ›**
    at its right, and shows them in the MODS view, scrolled to the source's first row. While the source modulates
    nothing, it reads **No modulations** and adds an empty modulation from the source there (see the MODS view),
    with its target's menu open.
    A random
    LFO has **Smooth** and **Chaos** knobs beside its display, which shows four made-up cycles of what they do;
    while notes play it shows the LFO's own last four cycles instead, swept across: each new cycle takes the next
    quarter, round from the right end to the left, the older ones fainter, with the live dot on the newest (at
    the end of a cycle, it waits there until the next one comes in). An LFO picks its shape from a list, like a
    shaper's curve, its button and each entry showing the shape's icon before its name. Under the shape list, as
    wide as it, are its clock modes side by side, as Poly | Mono are, the one it runs in lit, each drawn as an icon
    (named on hover): a rise that starts again at each dot for **Retrigger**, an infinity sign for **Free**, an
    anchor for **Anchor** and a single cycle that ends in a dot for **One-Shot**. A click picks one (a click on the
    one lit leaves it), and the wheel over them steps to the next one. Right of those two rows, centered
    on them, are the Rate and Phase knobs,
    over a random LFO's Smooth and Chaos, with the sync switch between the shape list and Rate, level with the list: a small square button
    with the metronome the effects' SYNC has, which turns the Rate knob into a note-value knob (as it starts, on).
    Under it, level with the clock modes, is the **Unipolar** switch, a hump on a baseline as a MODS row's
    polarity switch has.
    The Phase knob is faint while it does nothing (a random LFO, or a free one that isn't anchored), and otherwise
    a thin line on the display marks where notes start the LFO (or where the song starts an anchored one). A
    unipolar LFO's display has its zero line at the bottom. A Custom LFO's
    display, or the expand button in its title row, widens its card, to edit the shape there (see the shape editor).
  - A new modulation starts at +30 %, upward, whatever the target's setting. The target then shows a colored ring
    for the modulation range, plus a small dot of the same color under the
    knob (on a wire label or matrix cell, in its top right corner). Drag that dot to set the depth; double-click it
    to remove the modulation, or right-click it for a menu that types its depth, bypasses it, removes its aux (when
    it has one) or removes it. While the dot is under the mouse or dragged, its range (the ring, or
    the bar on a label, cell or depth bar) is drawn wider, over the other modulations' ranges where they overlap,
    and while its source's handle is on screen, a dashed line runs from the handle to the dot, as when the handle
    is hovered; with an aux, a dotted line runs from the aux's handle too. A source can modulate another source's depth too (drop its handle on
    a depth bar in the MODS view): the bar then has the dot in its top right corner, a strip under it shows
    how far the depth swings, and hovering the handle draws a dashed line to it like to any other target.
  - **More than fit**: a knob, label, cell or depth bar shows as many depth handles as its width holds (under a 30
    or 32 px knob 3, under a 34 px one 4; on a wire label 5, a matrix cell 4, a depth bar 11), in the order the
    sources show. Past that, as many as fit beside a **+n** pill (2 under a knob, 3 on a label, 2 on a cell, 9 on
    a depth bar) hold the rest, so nothing runs under the next control. Hovering the pill draws the ranges of the
    modulations it holds wider, and says how many there are; a hovered source whose depth handle the pill holds
    draws its dashed line to the pill, ringed in the source's color. A click opens the pill's list, under it (over
    it, short of room below), in the accent color while it is open: the target's name and every modulation of it,
    a row each: its bypass dot, the source's name, its aux (while one of the rows has one, the list is wider by a
    column for them: a chip in the aux's color with its name and an × that takes it away), its depth bar and its
    remove (right-clicking a row gives the depth handle's menu; hovering a row draws its range
    wider; a source's name too long for its row is drawn smaller to fit, down to about 9 px, and past that ends in
    "…"). A click anywhere else closes it, but not
    on a source's handle, so a handle can be dragged onto one of its rows. A source's handle held over a pill opens
    its list too; let go of on a row, the source becomes that modulation's aux (see below), and anywhere else the
    list closes again. The list closes with its pill, when the modulations come down to what fits. While every
    modulation the pill holds is bypassed or its source switched off, the pill is hollow.
  - **Aux**: a source's handle dropped on another source's depth handle, on a MODS row's aux cell, or on a row of a pill's list (while it
    is dragged, the depth handles and rows are ringed faintly, the one under it brightly) makes the dropped source
    that modulation's aux, in place of any it had; the depth handle then has a ring in the aux's color. A
    modulation's ring on its knob, and its range on a label or cell, show how far its depth and Amount take it,
    which an aux lets it go at most.
  - **Bypass**: the dot at the start of a modulation's row in the MODS view or a pill's list switches that
    modulation off and on again, keeping its target and depth, whatever its source (an LFO, an envelope, a macro, the
    note source, the mod wheel or velocity); so do **Bypass** / **Resume** in the right-click
    menus of the row, the
    knob's dot and the modulated knob. A bypassed modulation's dots are hollow, its row and ring dimmed. Bypasses
    are saved with the patch (as hidden params, like the drawn shapes).
  - A modulation's ring on a knob, and the bar on a label, cell or depth bar, show the range it swings through: both
    ways of the set value while its source is bipolar, or one way while it is unipolar.
  - While notes play, a white dot on each modulated knob shows where the newest voice currently is; with no note
    playing, it shows where the macros and the mod wheel take it, as they apply with or without notes.
  - Each depth bar (in the MODS view and a pill's list) has a white tick where its modulation takes its target now,
    on the bar's own scale, so a unipolar one at the top of its swing reaches the depth's handle: the newest voice's
    while notes play, and a macro's or the mod wheel's from its knob, notes or not. A modulation another source
    must run per voice shows none with no note playing, nor does a bypassed one or one whose source is off.
  - A modulated route shows it too: its wire (or its line to OUT), in its own color as any other,
    swells and thins with the modulation while notes play (with the macros' alone between notes), and its label or matrix cell has a small bar along
    its bottom with each modulation's range, a tick where the amount is set and a white dot where it is now.
- **Everywhere**: drag up/down to change values (hold Shift for fine steps, a knob then moving a fiftieth as fast; sliders such as the depth bars, the
  Mod wheel and the glow slider then move from where they are, a hundredth as fast; a value moves only once the mouse
  has moved more than 3 px, so a click, or a double-click's jitter, changes nothing), double-click to reset, use the mouse
  wheel (a trackpad's smaller steps add up to whole notches on what moves in steps: a stepped knob, Voices, an
  operator's pitch, the shape grid and the preset browser's list; over a switch, a notch up switches it on and one down
  off, as a click would, the keyboard switch and the symmetric switches with what they bring along, and over the
  partial counts it steps from one offered to the next; while something is being dragged, or a dialog is open, the
  wheel does nothing, a held key aside), or right-click for reset/remove options. An open list (an LFO's shapes, a
  MIDI source's inputs, an effect's or a band's choices) closes with undo and redo, and when its card scrolls out of
  view or no longer holds that type. A drag the editor loses the mouse during (another window taking it, or the
  browser demo's page its focus) ends where it is, its edit one undo step, but drops nothing: no route moves or goes,
  no modulation is assigned, no card moves. Hold the middle button and drag over a
  panel that scrolls (the effects chain, the source cards, the MODS view's rows, the preset browser's list) to drag
  what it shows along with the mouse, whatever is under it; past either end the row stops, and dragged back it
  follows at once. A middle press on an open list or menu does nothing, and one that starts a drag closes it;
  while the left button drags something, the middle one does nothing. A double-click whose first click closed the
  preset browser, a dialog or a list doesn't act on what was under it. Double-click the number a knob or a target's depth shows (under
  the knob, or beside it in the header), or pick **Edit value…** (**Edit depth…** on a modulation) from the
  right-click menu, or Ctrl+Click it, to type a value, in the units it shows or as a plain number (`300` on a
  time is 300 ms, `1.2k` on a cutoff 1.2 kHz; `Off` or `0` on a time that can be Off, while `1 ms` is the shortest
  time; on an envelope's attack, decay or release `0` is 0 ms and `0.5` half a millisecond, and `0` on a phaser's or flanger's Rate stands it still, while `0.02 Hz` is its slowest sweep); Enter or a click elsewhere sets it, Escape drops it. The editor
  takes the keyboard while you type and hands it back afterwards; the keys are taken straight off the window's
  message queue, so hosts whose message loops pass keys on to themselves (Bitwig) don't get them first. A wire
  label, OUT label or matrix cell types a value on Ctrl+Click, since its double-click removes or toggles the route.
- **Button rows**: buttons that pick one of a few (Poly | Mono, an operator's waves, a voice filter's and the filter
  effect's types, an LFO's clock modes, the partial counts, Points | Steps) and switches that sit side by side (the
  delay's Sync, Link and Ping-Pong, the phaser's and flanger's Sync and STEREO, the shaper's Symmetric and Link, the
  3-band dynamics' RMS and LINK) are drawn merged into one pill: one outline, rounded at its ends, with a thin line
  between its parts, each part lit on its own. A switch with no neighbor (Legato, Always, the power switches) stays a
  button of its own.
- **Keyboard strip**: along its top the **KEYBOARD** toggle, the macros close together, the note source and, at the
  right end, velocity; under them the
  wheels and six octaves of virtual keys, from C1 to C7 (the small − and + buttons under their right end, the
  range between them, shift them by an octave). The mod wheel's handle sits in the top row between the macros and the
  note source, its name under it, and the Bend Range knob (header-style: its label and value beside it) under the
  strip's title, over the bend wheel. The toggle (a chevron and the title, like the modulation panel's,
  as far from the strip's top) collapses the strip to its top row, taking the wheels, the keys and the octave
  shift away; the mod wheel's handle and the Bend Range knob stay, so the wheel can still be assigned and the range set. Whether it is collapsed is kept with the editor, like the modulation panel's, and loading a
  preset leaves it as it is. Drag across the keys to glide between notes. Keys light up while they are held, whether clicked here or
  played by the host. The **Bend** wheel bends the pitch by the Bend Range and springs
  back to center on release; the **Mod** wheel is the Mod Wheel param, and its handle in the top row drags onto what
  it should move (not the wheel itself), like a macro's (faint, like theirs, until it moves something). The wheel's and its handle's
  right-click menus make it unipolar or bipolar; bipolar, it fills from its middle, where it moves nothing.
- **Macros** (in the keyboard strip, above the keys): eight knobs, each in a color of its own, with a handle
  beside it to drag onto anything a source can modulate, just like an LFO's, and its name under it: double-click
  the name to change it (Enter
  keeps it, Escape doesn't, an empty name brings back "Macro 1" and so on; an undo step, saved with the patch).
  Hovering a handle draws lines to what it moves; the dots and rings on its targets work like a source's. Until a
  macro modulates something or scales a modulation as its aux, its handle, its knob's arc and its name are faint,
  and hovering the handle shows a "Drag to assign" tooltip (so do the note source's, the mod wheel's and
  velocity's). The right-click menu of its knob,
  handle or name makes it **Unipolar** or **Bipolar** (and the handle's or name's clears its targets); a bipolar
  macro's arc starts from the knob's middle, where it moves nothing, and a double-click on its
  knob (or Reset in the knob's menu) sets it back there, to 50 %, where a unipolar one's goes back to 0.
- **Note** (in the keyboard strip, right of the macros): the note source's handle, level with the macros', to drag
  onto a target like a macro's (and like a macro's, faint until it modulates something), and beside it the note
  the newest voice plays, as the source follows it, its name under it: the note's name, and
  the cents it is off while bent or gliding (a dash while nothing plays), over a faint picture of its curve. A
  click on it opens the curve's editor (see the shape editor). Its right-click menu, and its handle's, make it
  unipolar or bipolar (± in the display's corner while it is bipolar).
- **Velocity** (in the keyboard strip, at the right end, after the note source): the velocity source's handle, to
  drag onto a target like the note source's (faint until it modulates something), and beside it the velocity the
  newest voice came with, 0 to 127 as MIDI counts it (a dash while nothing plays), over a faint picture of its
  curve with a mark where that velocity falls on it, its name under it. A click on it opens the curve's editor (a
  second click closes it): velocity across, marked at 32, 64 and 96, and what the source puts out up, from nothing
  at the bottom to all of it at the top, with the straight line it starts as dashed behind and, while notes play, a
  dot where the newest voice is; its points and bends edit like the note source's curve, and its reset button
  brings back the straight line. Its right-click menu, and its handle's, make it unipolar or bipolar, as the note
  source's do.
- **Presets**: presets are `.fmv` files in `Documents\FMVibe\presets` and its subfolders. The first time the
  library is used (the browser, the arrows or a file dialog), FMVibe makes that folder with its factory presets in the
  `Factory` subfolder. Each name starts with two letters for its category, so the library lists them in groups:
  BA bass, FX effect, KY keys (bells and organs too), LD lead (brass too), PD pad (moving textures too), PL pluck
  and SQ sequence (rhythmic, gated). They are KY Electric Piano, BA FM Bass, KY Glass Bell, PD Warm Pad, KY Drawbar
  Organ, PL Pluck Delay; PD Crystal Motion, which uses everything at once (all six operators in three layers, two
  filters, four LFOs and a mod envelope, and six effects); BA Reese Bass, a heavy drum and bass reese (two stacks
  of detuned saws through a driven low-pass, a clean sub, clipping and three-band compression); BA Wobble Bass (an
  eighth-note LFO throwing open a resonant filter and the FM together); PD Shimmer Pad (unison layers through two
  filters, three LFOs and the shimmer reverb); SQ Trance Gate (supersaws chopped in sixteenths by a square LFO, a
  filter sweep over two bars); LD FM Brass (two stacks that blat in, an envelope on the FM,
  vibrato); FX Industrial Hit (a body dropping two octaves, band-passed noise, metallic partials and a sine-fold
  shaper); LD Folded Lead (a mono lead whose modulator goes through a sine-fold shaper on its way to the carrier,
  driven harder as each note starts); BA Fuzz Bass (an FM pair through an asymmetric shaper into a plucked
  low-pass, the shaper also growling back onto the modulator, over a clean sub); PL Comb String (a noise burst
  ringing a comb tuned to the note, on through a 6 dB low-pass; the mod wheel damps it); PL Chip Pluck (alternating,
  half and abs sines like the classic FM chips, the note source easing the FM off up the keyboard, a hard clipper
  going out beside it, and a Bite macro on FM Amount); SQ Step Filter (an organ wave and a pulse through a resonant
  18 dB low-pass that a drawn LFO shape opens on every eighth note, each its own height, into a cubic shaper; its
  Cutoff and Accent macros, the latter deepening the pattern); SQ Glass Steps (a custom wave from 64 partials, the
  Fibonacci ones, through a notch that jumps at random every sixteenth note); LD Vowel Lead (a vowel wave with a
  held bite and breath noise modulating it through a band-pass, gliding into every note, a bipolar envelope scooping
  into pitch and the mod wheel bringing in vibrato, a frequency shifter detuning the sides); FX Laser Zap (a
  one-shot LFO throwing the pitch down four octaves in a quarter second, a phase disperser and a staircase-curve
  shaper crushing it; its Speed and Grit macros); FX Noise Riser (wide noise through a high-pass and a climbing
  sine, swept evenly over eight seconds by an envelope, turning a frequency shifter's spiral faster); KY Temple Gong
  (three pairs at inharmonic ratios, one of them modulated by a fixed 137 Hz operator, so each key grinds
  differently, the modulators dying before their carriers and velocity driving every index); LD Feedback Lead (a
  mono lead whose oscillator is a self-modulating operator, the mod wheel opening its feedback and index from flute
  to buzzing reed, a mod envelope's bite on each note and a vibrato that a second envelope, as its aux, fades in); and
  PD Six-Op Cascade (all six operators in one chain, F down to A, four slow LFOs breathing its links at rates that
  never line up, and an Index macro deepening them all). A folder made
  before an example came gets it the next time the library is used; `%APPDATA%\FMVibe\settings.txt` counts the examples
  given, so deleted ones stay gone. The plugin carries the factory presets built in and writes them out as they are:
  they hold only the params that differ from Init, so their files are small, and load like any other.
  - The **preset bar** in the middle of the tabs' row shows the preset loaded or saved last, or **- Init -** (as
    a new instance does), with a dot while the patch has changed since. Its arrows step to the previous or next
    preset in the folder, round from the last to the first. Clicking the name opens the **browser**: the presets
    at the top of the folder, then each subfolder's under its name, the current one marked; a click loads one
    and closes the browser (if the patch has changed, it asks first, over the browser, which stays open when the
    load is called off); **OPEN FOLDER** shows the folder in Explorer, and the × or a click around it closes it.
    While it is open, the browser takes the keyboard for its **search**, in its title row: what is typed narrows
    the list to the presets whose subfolder and name hold every word of it, whatever the case, and the count
    beside the title says how many match. The first one found is picked (outlined); Up and Down pick another
    from the list (from the preset loaded, before anything is picked), and Enter loads the one picked. Escape, or
    the × in the search, empties it; Escape with nothing typed closes the browser. The search starts empty each
    time the browser opens. It edits like a text field: Left and Right move the caret, Home and End jump to the
    ends, and with Shift they select; a click puts the caret there, a drag or a Shift+click selects, a
    double-click picks a word, and Ctrl+A selects it all. Typing replaces the selection, Backspace and Delete
    take it out, and Ctrl+C, Ctrl+X and Ctrl+V copy, cut and paste (line breaks pasted as spaces, up to 40
    characters in all). A search longer than the field scrolls to keep the caret in sight. A value typed into a
    knob copies and pastes too, while all of it is selected.
  - **Init** sets every param and the routing layout back to their defaults (the zoom stays). **Load** and
    **Save** read and write a `.fmv` file through the standard file dialog, which starts in the preset folder;
    Save suggests the current preset's name. The file holds the same data as the host's saved state, apart from
    the settings.
  - Init and loading ask first only while the patch has changed since it was loaded, saved or reset, or when the
    host brought a patch that isn't a preset, since the patch they replace can't be undone back (undoing every edit
    since a preset loaded leaves it unchanged again). The project remembers which preset it was and whether it changed.
- **Settings** (the cog after Save): **Oversampling**, 1x, 2x, 4x or 8x. It is saved with the project but not
  with presets, and changing it restarts the plugin (the host deactivates and reactivates it).
  **MPE**, Off or On, and its **Bend** range for the member channels, 12, 24, 48 (to begin with) or 96 semitones,
  faint while MPE is off (see MPE, above). Like Oversampling, they are saved with the project but not with presets,
  and aren't undo steps.
  **Theme**: Midnight (the default), Graphite (neutral grays), Light (crisp: white cards and knob caps on a cool
  gray, with deep colors) or Paper (warm: cream on sand, with ink-brown text and earthy colors), each button
  shown in its own colors. **Glow**, a slider from Off to 100 % (50 % to begin with), makes the curves in the
  graphs (envelopes, LFOs, filter and effect responses) glow, drawing them a little thinner: the higher, the
  stronger and wider the halo.
  The editor shows it while it is dragged and keeps it when it is let go. **Lines**, a slider like it from Off to
  100 % (75 % to begin with), sets how strongly the dashed lines from a source's handle to what it modulates (and the
  one a handle being dragged trails) are drawn: Off draws none, 100 % draws them opaque. **Handles**, Small (to begin
  with), Medium or Large, draws the depth handles and their **+n** pills bigger, each step about half a pixel more in
  radius and a pixel more across, in the same places (so on a label or cell, Large ones overlap a little). All four
  are yours rather than the
  project's: they are kept in `%APPDATA%\FMVibe\settings.txt`, every FMVibe editor uses them, and other open editors
  switch along. An editor writes only what it changed into that file, leaving what another one changed meanwhile. The dialog's bottom line shows the version number and that FMVibe is released under the MIT
  license.

### Remote-control pages (Bitwig, CLAP)

| Section | Pages |
| --- | --- |
| Mix | Macros (the eight knobs, the page shown first), Global (Volume, FM Amount, Pitch, Pan), Play (Mono, Legato, Glide, Glide Always, Bend Range, Voices), Outputs (A–F, then S1 and S2 To Out) |
| Operators | Op A … Op F: Wave, Ratio, Pitch (Fixed Freq and Fixed Mult while the operator is fixed), Detune, Attack, Decay, Sustain, Release |
| Filters | Filter 1 and 2: Type, Cutoff, Resonance, Key Track, Drive, Out, On, Slope; Filter links: F1 Feedback, F1 → F2, F2 → F1, F2 Feedback |
| Shapers | Shaper 1 and 2: On, Curve, Drive, Output, Mix, Link, then what it feeds into F1 and F2 |
| FX | One page per effect in the chain, named after its place and type, with that effect's knobs (a shimmer reverb's Pitch and Shimmer in place of its Pre-Delay and Width) |
| Matrix | "A modulates" … "F modulates": what that operator modulates; "Into F1", "Into F2": each operator's send into that filter; "F1 modulates", "F2 modulates": the operators each filter modulates; "Into S1", "S1 modulates" and the same for S2: each operator's and filter's send into that shaper, and the operators it modulates; "Shapers into shapers": S1 Feedback, S1 → S2, S2 → S1, S2 Feedback |
| Modulation | One page per source, in the editor's order: an LFO's shape, rate, sync and division, or an envelope's attack, decay, sustain and release, then its 4 slot depths |

## CLAP and VST3

FMVibe comes as a CLAP plugin (`FMVibe.clap`) and a VST3 plugin (the `FMVibe.vst3` folder), built from the same code:
the same engine, editor, parameters (with the same ids), presets and settings. The VST3 plugin is written straight
against the VST3 interfaces, with no wrapper in between, as one object that is both its processor and its edit
controller. Its state is the same bytes as the CLAP plugin's, so a `.fmv` preset loads in either; hosts see them
as two different plugins, though, so a project made with one doesn't switch to the other by itself.

What the VST3 plugin does its own way:

- **MIDI**: VST3 hands plugins notes, but no controllers. FMVibe asks hosts to map every controller, channel pressure
  and pitch bend, on each of the 16 MIDI channels, to hidden parameters of its own, and takes them as MIDI from
  there: pitch bend (by Bend Range), the MIDI sources' controllers and pressure, and MPE over MIDI work as in the
  CLAP plugin. Polyphonic pressure comes as its own event, a note's pressure.
- **Note expressions**: Volume, Pan, Tuning (ten octaves either way) and Brightness (a note's slide), as VST3 names
  them, and Pressure, one of FMVibe's own. For touch surfaces and MPE controllers, FMVibe tells hosts to send a
  finger's sideways movement as Tuning, its movement along the key as Brightness and how hard it presses as
  Pressure. A note-on's tuning (in cents) tunes the note from its start.
- **Parameters** are grouped in units, as their modules are (Operators, Op A, Filters, ...). The fields of effects
  and sources a slot or place doesn't hold are hidden, as in the CLAP plugin, and the host is told to look again
  when they change. Parameters hosts don't show are read-only to them.
- **Restarting**: raising the voice limit past the voices allocated (a project loaded may too), or changing the
  oversampling, asks the host to deactivate and reactivate the plugin (through a latency change, which has hosts do that).
- **Not in VST3**: CLAP's per-voice and global host modulation, Bitwig's remote-control pages, and the note-end
  reports that per-voice modulation needs.
