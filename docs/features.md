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
    octave) to brown (−6 dB per octave) at about the same loudness, and **Spread** (the Unison Spread param) widens it
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
  - Out (level straight to the audio output) and sends into the two filters and the two shapers, set in the
    routing graph and the matrix;
  - **Pan**: where the operator sits in the stereo field, L100 to R100 (centered by default), equal-power and at
    full level in the middle, as the global Pan. It places all the operator puts out, to the output and into the
    filters and shapers (which are stereo, so a panned operator stays where it is through them), and its unison
    copies' spread with it; what other operators hear of it is the mono sum, so a modulator's Pan changes nothing it
    modulates, and does nothing unless the operator is also heard;
  - **Phase**: where a note starts the waveform, 0 to 100 % of a cycle (0 % by default; a sine at 25 % starts at
    its peak). It is read as a note starts, with the host's and the sources' modulation on it then, and holds for the
    note. A noise operator has none. Operators don't follow velocity themselves: velocity reaches them as a source
    (see Velocity, under the modulation sources), aimed at Out or wherever else;
  - **Unison**: up to 16 copies of the operator. *Detune* (Unison Detune) spreads them in pitch (the outermost
    copies by up to ±200 cents; patches saved while it stopped at 100, state version 12 and earlier, load a
    modulation's depth on it half as deep, to swing it as far) and *Spread* (Unison Spread) across the stereo field. On a modulator, the copies' sum
    is what its targets hear; on a carrier, every copy is modulated by the same signal. How they spread is the
    operator's to pick (`src/unison.h`; to begin with, evenly in pitch and all as loud, as unison always played, and
    Alternate across; patches saved before these, state version 12 and earlier, load Ramp, from left to right, as
    they played):
    - **Unison Curve**, where the copies go in pitch: *Linear* (evenly), *Center* (bunched about the pitch, the
      outermost as far out as Linear's, each copy's share of the detune its even share squared), *Supersaw* (as a
      classic supersaw spaces its seven: unevenly, the outer pair far out; other counts read along those seven) or
      *Random* (one copy anywhere in each of as many even stretches of the detune, drawn new for every note and
      each operator, held through the note);
    - **Unison Pan**, where they sit: *Ramp* (from left to right as their pitch rises), *Alternate* (to begin with:
      copies next to each other in pitch on opposite sides, mirrored outward from the middle copy, so the stereo field is wide
      however few copies play, and no two sit in one place) or *Random* (the even places dealt out new for every
      note);
    - **Blend** (Unison Blend, 0 % to begin with): turns each copy down by how far out its detune takes it, the
      outermost to a tenth of the middle at 100 %, all together kept as loud as before. It acts on what a modulator's
      targets hear too, so a detuned modulator can keep a strong middle with a quiet halo.

    Nothing changes for Noise, which has no copies.
  - **Note Sync** (hard sync, off by default; not the Sync warp below): each cycle of the note's pitch restarts
    the operator's wave at its Phase, as an analog oscillator synced to a master at the note. The operator's ratio
    (or its fixed Freq × Mult, Pitch and Detune) then sets the pitch inside each of those cycles, while what sounds
    stays on the note's harmonics, so a ratio swept by an envelope or an LFO is the classic sync sweep. Every
    operator can sync, carriers and modulators alike: a synced modulator gives its carrier a spectrum tied to the
    note whatever its ratio. The note is the master rather than another operator because it is there for every
    operator in every patch, doesn't depend on a routing, and is what sync is played against; a Sync From an
    operator could be added to its list later (its name says what it syncs to, as the Sync warp syncs the wave to
    the operator's own cycle). Its unison copies each sync to a note detuned as the copy is, and the note's pitch
    follows bend, glide and tuning. A restart's step, and the change in the wave's slope there, are smoothed over
    the samples either side as a band-limited restart would be (polyBLEP and polyBLAMP), so a synced sine at C6
    keeps what aliases 65 dB down at the default 2x oversampling. Its scope (on the WARP tab) follows the note's
    cycles. Noise has no cycle to sync or warp;
  - **Warp** (Off by default) and **Warp Amount** (−100 to +100 %, 0 to begin with): where along its cycle the
    operator reads its wave, any wave, Custom too (Vital-style phase warps):
    - **Bend**: rushes through the cycle's start and lingers at its end (below 0, the other way round), as Casio's
      phase distortion bends a sine toward a saw;
    - **Squeeze**: the cycle's first half in less of it (below 0, more) and its second half over the rest: a
      square's pulse width, from 50 % to a sixteenth either way, a sine or a saw leaning;
    - **Pulse**: the whole cycle squeezed into its start, resting where it began for the rest (below 0, resting
      first and the cycle at the end);
    - **Steps**: the cycle in 8 steps (below 0, 4), each holding, then rising to the next;
    - **Sync**: the wave played up to 8 times through each cycle (1 + 7 × the amount's size, either way), cut off
      where the cycle ends, as Serum's Sync warp: an oscillator hard-synced to the operator's own pitch, its formant
      sweeping up as the amount rises while the pitch stays. At whole numbers of times (2/7 is 3 times) it plays
      that harmonic alone. Its step at the cycle's end is smoothed as Note Sync's restart is, so a sine at C6 keeps
      what aliases 80 dB down at 2x (a saw, cut through its own edge, 55).

    At 0 every warp is the plain wave, bit for bit; the most it squeezes a stretch is 8 times, at ±100 %. A warped
    operator reads a table with as many fewer harmonics as its warp squeezes the cycle at its steepest, and the
    bends a warp puts in the wave's slope are smoothed as a sync's restart is, so even Steps at C6 keeps what
    aliases about 60 dB down at 2x. Warp Amount can be modulated (pulse-width modulation is an LFO on Squeeze); it
    glides to each control block's value, moving at most its whole range in 8 ms, and a warp changed to another
    first goes back to the plain wave and then out to the new one, so neither clicks. Warp and Note Sync work
    together: the restarted wave is read through the warp. An operator with neither runs the plain loop it ran before,
    the synced and warped ones a loop of their own;
  - **Rnd Phase** (phase randomization): at 0 % (the default) every note starts the waveform, and each unison copy, at
    Phase, so notes sound alike from the first sample; turned up, each copy starts up to that much of a cycle
    later, at random. Unison copies at 0 % start in step and drift apart as they detune.

  Waves come from band-limited wavetables with one table per octave, so saws and squares stay clean high up the
  keyboard, even when they are modulators.
- **Matrix**: `X → Y` sets how strongly operator X modulates operator Y, and `X Feedback` is X modulating itself.
  Any operator can feed any other, cycles included. In each sample, earlier operators (A before B) pass on their
  current output and later ones their previous output; feedback is smoothed over two samples. Amounts are squared
  internally, so the low end is fine-grained. **FM Amount** scales the whole matrix.
- **Route modes**: each route into an operator, from an operator, a filter or a shaper, acts on it in one of four
  ways, Phase to begin with:
  - **Phase**: the source moves where the operator reads its wave (phase modulation, what FM synths call FM). Up to
    8 radians at 100 %.
  - **Linear FM**: the source is added to the operator's frequency, up to 4 times its own frequency either way at
    100 %. Past 50 % the frequency swings through zero and the operator runs backwards for a moment. The mean
    pitch stays where it was, and a source with a DC offset (a shaper's asymmetric curve, a slow filter) detunes it.
  - **Exp FM**: the source moves the operator's pitch by octaves, up to 4 either way at 100 % (one at 50 %), as a
    control voltage into an analog oscillator does; the deeper it goes, the higher the mean pitch.
  - **Ring**: the operator's output is multiplied by the source: at 100 % only the sum and difference frequencies
    sound, at 50 % the operator stays at half its level with them beside it, as amplitude modulation.

  Like Phase, Linear and Exp FM take their amounts squared, and all four are scaled by FM Amount. An operator's
  feedback is always Phase. An operator that Linear or Exp FM can take higher reads a wavetable with fewer
  harmonics, so it stays below Nyquist at the top of its swing. The mode is a param of its own for hosts (`B → A
  Mode`), stepped and automatable, and changes at once.
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
  Drive (saturation before the filter, anti-aliased like the shapers), and **Mix**, which blends what goes in (before
  Drive) with what the filter makes of it, as the filter effect's does (100 % to begin with, all filter): everything
  the filter puts out, to the output, the operators, the filters and the shapers, is that blend. A voice only ends once its filters have
  rung out (a plucked comb through the silences between its pulses too), and a filter whose sends go to 0 rings
  out too; when they come back, or its type or slope takes up a stage it left out, that starts from silence.
- **Shapers S1 and S2**, per voice, each switchable on and off (off to begin with): the shaper effect in every
  voice, with its curves (Hard, Soft, Asymmetric, Cubic, Gentle, Sine Fold, Fold, or a Custom curve drawn like the
  effect's, the S it starts as included), its Drive (0–36 dB), Output, Mix, Link (on to begin with) and Symmetric
  (off), and run by the same code: anti-aliased the same way, a drawn curve gliding to moved points and its level at the middle taken out the same way, with the same DC blocker after the uneven and drawn curves. Any operator,
  filter or shaper can be sent into a shaper (`A → S1`, `F1 → S1`, `S1 → S2`), a shaper into itself too (`S1
  Feedback`), and a shaper's output feeds the filters (`S1 → F1`), for
  distortion into a filter or a filter's sound distorted, and modulates operators the way an operator does
  (`S1 → A`, scaled by FM Amount), so a modulator can be folded or squared off before it reaches its carrier. Its
  **To Out**, set by its line to OUT's label in the routing graph, sends it straight to the output, like an operator's Out (0 to begin with, so a new shaper is heard only
  once it is routed out, or through what it feeds or modulates). Operators hear its previous output sample, and it
  hears a filter's previous one, so a filter and a shaper can feed each other round in a loop. S2 hears S1 as it is
  that sample, and S1 hears S2, and each shaper itself, a sample late, so shapers can feed each other and themselves
  round in a loop too. Once a released note's operators have faded, what the shapers get from the filters and the
  shapers fades away, so such a loop can't ring on. What a shaper hears from a filter or a shaper is held within
  +18 dB, so a loop that lifts itself past unity (a resonant filter, or shapers feeding back, with a shaper's Mix
  below 100 %) grows loud but never without end. It runs only while something goes in and its output goes
  somewhere (a shaper feeding only itself goes nowhere; one feeding a shaper that goes somewhere does).
- **Modulation**: up to 16 sources, any mix of LFOs (sine, triangle, saw or square; 0.02–100 Hz,
  and at the Rate knob's bottom 0 Hz, where an LFO stands still at its Phase), mod envelopes (AHDSR, starting at full sustain and with an operator's 15 ms release; times as an
  operator's envelope has them), Curve sources (LFOs of a drawn shape), and two that follow what a note or the
  controller brings:
  - **Curve**: an LFO of a shape drawn from points (see below), with everything an LFO has (Rate or a synced note
    value, the clock modes, anchoring, Phase) but the shape list. It starts unipolar, where an LFO starts bipolar.
    It replaces the LFO's Custom shape, which the LFOs no longer offer: a patch saved before (state version 11 and
    earlier) loads an LFO with that shape as a Curve source running as the LFO ran, its points and polarity kept.
  - **Random**: a random LFO. It runs as an LFO does, with the LFO's Rate (or, synced, a note value: it starts synced
    in quarter notes, as an LFO does), and takes a new value every cycle. Of the clock modes it has only
    **Retrigger** and **Free**: its values can't be told ahead, so standing where the transport puts it or running
    one cycle means nothing to it (a patch or host that sets its Anchor runs it free, and its One-Shot retriggers it). **Chaos** sets
    how far it may move from the last one: up to a tenth of the range either way at 0 %, so it wanders, rising to
    anywhere at all at 100 % (the default), like sample and hold. **Smooth** glides to each new value instead of
    jumping: over that share of the cycle, easing in and out, then it holds (0 %, the default, steps). Unsynced at
    0 Hz, the Rate knob's bottom, it stands still: each note takes a value of its own as it starts (and as it restarts
    it, in mono without legato), anywhere at Chaos's 100 %, at once, and holds it, so notes each detune, brighten or
    pan a little differently. It takes no Phase. It replaces the LFO's Random shape, which the LFOs no longer offer:
    a patch saved before (state version 10 and earlier) loads an LFO with that shape as a Random source running as
    the LFO ran, and a Random source, which then only took a value for each note, as one at 0 Hz.
  - **MIDI**: what its **Input** picks. **CC**, to begin with: a MIDI controller, **CC** 0–127 (1, the mod wheel's,
    to begin with), the same for every voice. **Pressure**: aftertouch, a voice following its own note's (MIDI
    polyphonic aftertouch, CLAP's pressure note expression, or channel pressure on an MPE member channel) once some
    has come, and the channel's (MIDI channel pressure) until then. **Slide**: a voice's own note's slide, the
    finger's up and down movement an MPE controller sends (CLAP's brightness note expression, or CC 74 on an MPE
    member channel), resting in the middle until some comes: bipolar, it moves nothing at rest. It follows its
    input through **Slew**, a time constant from 1 ms (all but at once, to begin with) to 10 s, starting each note
    where its input is.
  A new Random source starts bipolar, as an LFO does, and a MIDI source unipolar. The browser demo takes no MIDI, so there a MIDI source stays at 0. A new
  instance has none; each one added starts
  switched on, and a source that is switched off stands still and modulates nothing, but keeps its targets. Each
  has 16 slots, and a slot can
  target any continuous parameter: a knob, a matrix amount, an operator output, even another LFO's rate or a slot
  depth. Depth is ±100 % of the target's range: how far a **unipolar** source takes the target one way, and the
  whole swing of a **bipolar** one, half of it below the set value and half above (at 100 %, −50 % to +50 %).
  Every source can be either, for all of its modulations, picked from its right-click menu or the switch after its
  name in the MODS view (see the editor): LFOs, Random sources and the pitch bend start bipolar, and Curve sources, envelopes, macros,
  the note source, the mod wheel and velocity unipolar. A bipolar envelope swings about its middle level (nothing at half of
  it, half the depth down at rest and up at its peak), and a bipolar macro, mod wheel, note source, velocity or pitch
  bend about the middle of what it puts out (a macro moves nothing with its knob at 50 %, the pitch bend nothing at
  rest). LFOs and envelopes run
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
  (an LFO, an envelope, the note source, velocity, the pitch bend) scales it in each voice by its own output there, as
  of the last 32-sample block (or this one, for a source that comes before it, and velocity and the pitch bend, known
  as the block starts; on a note's
  first block the note source's comes from the pitch the note starts at); a macro or the mod wheel scales it the same for
  all. A macro's or the mod wheel's modulation with an aux that runs per voice then runs per voice too, reaching the
  effects through the newest voice. A switched-off aux puts out nothing, so it scales its modulation to nothing; a
  source can't be its own modulations' aux. Once a key is
  let go, a mod envelope that modulates something runs its release out even after the operators have faded: the
  voice keeps running its modulation alone (rendering no sound) until the release is over, so what it moves, an
  effect's knob for one, follows it down instead of jumping back, and only then reports its note's end to the host.
  Once the last voice has ended, its modulation of the effects (and of Volume) runs on while they ring out, so an
  LFO on a filter after a long reverb keeps sweeping the reverb's tail: a Free or Anchor LFO along its shared clock,
  a Retrigger or One-Shot one carrying on from where the voice left it, its own params as every voice has them
  (what is set, with the host's and the macros' modulation); the other sources' modulations keep what they last
  gave. Once the effects have put out silence (below -100 dB) for a second, the effects' params go back to their
  knobs, where nothing is heard, and a new note takes over from its first block. The voice itself still ends, and
  reports its note's end, as it did. Meanwhile an LFO that modulates the effects this way shows running on its card,
  its dot moving along as it did under the note (a Random source's glides too), until the tail stops.
  - **Macros**: eight knobs (0–100 %) that hosts can automate, each with 16 slots like a source's. A macro's
    value times each slot's depth is added to its target for every voice and the effects alike, so a macro can
    also move effect params, which per-voice sources can't. Their names are part of the patch.
    A macro's knob is a target too: any other source can modulate it (an LFO, an envelope, the note source,
    velocity, the pitch bend, a Random or MIDI source, the mod wheel, or another macro), and what the knob then puts
    out, in its range, moves what the macro modulates and scales the modulations it is the aux of. The macros and the
    mod wheel moving a macro's knob do so for every voice and the effects alike, along a chain of macros too (they
    are worked out in the order the chain needs, whatever their numbers); a macro can't modulate its own knob, nor
    the knob of a macro its own knob moves, which would make a loop (a loop a host or a state holds anyway is broken
    where it closes: the macro read there that comes later is read as set). A source that runs per voice on a macro's
    knob makes that macro, and the macros its knob moves, run per voice: each voice works its knob out for itself,
    as of the same 32-sample block (the note source's from the block before, as for an aux), its modulations moving
    that voice and reaching the effects through the newest voice, as a per-voice source's do; with no note playing
    the macro moves its targets from its knob with the macros' and the mod wheel's modulation alone. Turned by the
    host or by hand, the knob's own value is what the modulation is added to.
  - **Mod Wheel**: a macro of its own, its knob the Mod Wheel param (the wheel in the keyboard strip), with 16
    slots like a macro's. It moves nothing until something is assigned to it. A hardware mod wheel (MIDI CC 1, on any
    channel; in a VST3 host, through its MIDI mapping) moves it as the wheel in the strip does, and the host is told
    of the param's new value. Like that wheel's, its position is played rather than part of the patch: nothing to
    undo, and the preset stays unchanged. A MIDI source on CC 1 follows the hardware wheel too.
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
  - **Pitch Bend**: a source that puts out where the pitch bend stands, from all the way down to all the way up, its
    middle at rest: the host's MIDI pitch bend and the **Pitch Bend** param (the editor's bend wheel) together and,
    with MPE on, a note's own channel's bend on top (each as a share of its own range, whatever Bend Range says). Like velocity it has 16 slots
    and no switch, runs per voice (so with MPE each note follows its own bend), reaches the effects through the newest
    voice, and modulates from a note's first block on. It starts bipolar, so it moves nothing at rest and swings its
    targets both ways as the wheel goes up and down; unipolar, it moves them half its depth at rest and all of it fully
    up. The pitch still bends by Bend Range too, whatever the source modulates.
  - An LFO runs in one of four clock modes, picked one at a time: **Retrigger** (the default), **Free**, **Anchor** or
    **One-Shot**. Retrigger restarts the LFO from its Phase with every note. Each of the other three is a switch
    hosts see; the editor turns the other two off as it picks one. Should a host turn on more than one, One-Shot
    wins, then Anchor. A Random source has only Retrigger and Free (see Random above).
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
    its peak, and 100 % is the cycle's start again. A free LFO keeps its shared clock, so it doesn't use it (nor does
    a Random source, which keeps its steps); an anchored one stands at it at the song's start. A one-shot runs from there to the cycle's
    end, so at 50 % it plays only the second half.
  - **Sync** locks an LFO to the host tempo: its rate becomes a note value from 4/1 down to 1/64, each straight,
    dotted (D) or as a triplet (T). Without a tempo from the host, it assumes 120 BPM. The browser demo has no
    host: its editor's title bar has a **tempo** field instead (20–999 BPM, 140 to begin with, remembered by the
    browser; see Header), which every synced LFO, delay and sweep follows, as does `fmvibe-uihost`'s. A new LFO
    starts synced, in quarter notes. The note
    value takes modulation like a knob, from the editor's sources, but in steps: it moves from one
    note value to the next along the list (up for faster), never between them, and the dot on its knob stands on
    the one playing.
  - **One-Shot** runs an LFO through one cycle from each note and then holds it where the cycle ends (a one-shot
    is never free or anchored): for a sine, a triangle or a drawn shape that is where it started, while a saw holds
    at its top, a square low and a Random source at the value it took. A drawn shape with a point at the cycle's end
    (100 %) holds at that point's level instead, so a ramp from 0 up to 1 there stays at 1.
  - **Unipolar** makes an LFO swing from 0 to 1 instead of −1 to 1, so it only pushes its targets one way, like an
    envelope. It is the same choice the LFO's right-click menu offers, and its display then has its zero line at the
    bottom.
  - A Curve source's shape is drawn from 2 to 64 points, starting as a triangle: up from the bottom to the top at the
    middle of the cycle and down again. Each segment between two points can be bent, from easing in
    to easing out, and the last segment wraps around to the first point, so the cycle loops without a jump. The
    points are saved with the patch (as params of the editor's own, which hosts aren't given: see
    [CLAP and VST3](#clap-and-vst3)).
- **Effects**, after the voices: everything the operators and filters send to the output runs through up to 32
  effects. A new instance starts with none. Any effect can be added more than once, switched on and off, reordered and
  removed. A patch saved while the chain held 16 loads its effects in the order they had. Once nothing goes in and
  every tail has rung out (a compressor's or a 3-band dynamics' release run out too), the effects rest, costing
  next to nothing, with the editor open too (the analyzers taking in the silence, so their spectra fall away and
  the graph's glow fades); a phaser's, flanger's or chorus's sweep goes round meanwhile, so a synced one keeps
  time.
  - **A line with splits**: the effects run along a line from the voices to the output, a chain of them one after
    another, which up to four **splits** cut by frequency into two bands at their Crossover (250 Hz to
    begin with), at a Slope of 6, 12 or 24 dB per octave (12 to begin with, a second-order Linkwitz-Riley crossover; 24 the
    fourth-order one the 3-band dynamics has; 6 a one-pole low-pass with the rest as the high band), so the bands sum
    back flat at each. A split can also cut into **three** bands, low, middle and high, at a low and a high crossover
    (250 Hz and 2.5 kHz to begin with, the high one kept a third of an octave above the low one: in the editor, either
    crossover moved past the other pushes it along, in the same gesture and undo step), at 12 or 24 dB
    (24 as the 3-band dynamics does; at 12 the low band goes through the high crossover's all-pass, so the three still
    sum back flat; set to 6, it plays at 12). Each band runs through effects of its own, say the lows through a shaper and a compressor and the
    highs through a delay and a reverb, until a **join** adds them back together, each at a level (0 dB, full, down to
    -60 dB, silence; full to begin with), and the line goes on from it; a split left open at the line's end sends both
    bands out. The join can **solo** bands, each on its own, letting only those soloed through. The splits
    and joins don't take any of the 32 effects' places, and modulation moves a split's crossover and a join's levels
    like any knob. The editor shows the line two ways, as a row of cards or as a graph (the FX tab's switch, see the
    editor), and switching between them changes nothing: underneath, the line is the effects graph's wires between
    the effects, which both views lay along it and the effects always run as. A
    wire's level glides as it moves, a wire connected or taken away fades in or out over 5 ms, a wire moved
    crossfades from where it came from to where it comes from now over the same 5 ms, and a wire that would loop
    back (which the editor doesn't make) reads what its source put out 64 samples before. Effects moved along the
    line, which would for a moment make the wires crossfading read each other in a loop (two effects swapping places
    each reading the other), duck the output out over 3 ms instead, take their places at once, and bring it back in
    over 3 ms. A node leaving the graph
    (an effect removed, a split taken out with its join) keeps playing what goes into it until the wires out of it
    have faded, so the sound moves to its new path without a click or a gap; one coming into it starts from what goes
    into it, as the wires into what follows fade in. A split's slope changed crossfades from its filters at the old
    slope to fresh ones at the new over 5 ms. Only what the output hears runs: an effect no wire takes on to the
    output costs nothing, and starts again cleared once one does. A patch starts with no effects, its input straight
    out, and one saved while the effects ran as a chain loads that chain as the line.
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
  - **Amp**: a guitar amp, then a speaker cabinet heard through a mic. **Model** picks the amp: its preamp stages
    (each a coupling high-pass, a gain, a curve with a bias whose lower side is softer, which adds even harmonics, and
    a low-pass), its tone stack and its power amp. **Clean** (one gentle stage, lots of headroom), **Blues** (two
    stages, a loose low end, breaking up early, and plenty of sag), **Crunch** (two stages, tighter and mid-forward;
    the one a new amp starts as), **Lead** (three stages, each cutting a little more top, for a smooth sustain),
    **Heavy** (four stages behind an input that lets nothing below about 500 Hz through, high gain, little sag),
    **Bass** (one stage for grit, the lows under 250 Hz blended in clean under it) and **Fuzz** (a fuzz pedal, two
    hard, lopsided stages, into a clean amp). **Gain** drives the preamp over its model's range (Clean about −6 to
    +20 dB, Heavy +18 to +50 dB). **Bass**, **Mid** and **Treble** set the tone stack: for every model but Bass the
    classic passive three-knob circuit, worked out from its parts (our own values) as a third-order filter, so the
    knobs interact as an amp's do (Mid at 0 scoops; the bass pot's taper is log); its loudest at the middle knobs is
    0 dB. Clean's and the Fuzz's amp's are scooped, Blues', Crunch's and Lead's mid-forward, Heavy's scooped deeper;
    Bass has an active stack instead, a ±15 dB shelf at 90 Hz, a ±12 dB bell at 600 Hz and a ±15 dB shelf at 3 kHz.
    **Presence** lifts a shelf around 3–4 kHz into the power amp, up to 10 dB. **Volume** drives the power amp, whose
    headroom sags as it works (falling over 5 ms, coming back over 150 ms, by as much as its model has) for a bloom.
    The knobs from Gain to Volume read 0 to 10, as an amp's; their coefficients follow them every block, gliding over
    20 ms. Each model is trimmed so that at its middle knobs a sine at −12 dBFS comes out as loud as it went in (with
    the cabinet), and as Gain rises the level grows less and less, the stages clipping. The stages run at 1, 2, 4
    (to begin with) or 8 times the host rate, picked in the card's right-click menu (**Oversampling**), and come back
    down through the same half-band filters as the voices, with no latency: at 4x, Heavy at full Gain and Volume on a
    C7 keeps what isn't a harmonic of the note about 42 dB down (20 dB at 1x, 63 dB at 8x). **Stereo** (off to begin
    with) runs an amp for each side; off, one amp runs on their sum, at half the cost, and puts it out on both.
    The **cabinet** (on to begin with) comes after it, at the host rate: **1x12 Open**, **2x12 Open**, **4x12 Closed**
    (to begin with), **4x10 Bass** or **1x15 Bass**, each a resonant low end (open backs looser, closed ones tighter,
    the bass cabinets lower), a low-mid bump, the cone's breakup peak and a dip below it, and a low-pass at 24 dB an
    octave (3 to 5 kHz). The **mic** in front of it is **Dynamic** (a presence peak, a little low cut; to begin
    with), **Condenser** (flatter, more air) or **Ribbon** (dark and smooth, more low end); **Position** moves it from
    the cone's center (0 %, bright) to its edge (100 %, the top and the breakup peak down), 30 % to begin with, and
    **Distance** from 1 cm to 1 m (2 cm to begin with): close in, its proximity effect lifts the lows; from about
    15 cm on the room comes in, three early reflections (1.3, 3.1 and 7.7 ms, darkened), all of them at 1 m. Off,
    the amp goes straight on. The cabinet and mic are filters, not recordings, so they move smoothly as Position and
    Distance are turned or modulated. **Output** (−24 to +12 dB) and **Mix** come last, gliding sample by sample.
    Changing the model, the cabinet, the mic, switching the cabinet on or off, Stereo or the oversampling crossfades
    from the amp as it was to one at the new setting over 10 ms, so it neither clicks nor dips (another change during
    those 10 ms waits for them to end).
  - **Delay**: separate times for the left and right side (1 ms–2 s each) or, synced, a note value of the host
    tempo for each (up to 4 s); Feedback (up to 95 %); Tone, a low-pass below the middle (20 kHz down to 200 Hz)
    and a high-pass above it (20 Hz up to 4 kHz), applied to every repeat; Width, from the echoes' full stereo
    down to mono; Ping-Pong, which starts the echoes on the left and bounces them across, each after its side's
    time; and Mix, where the dry signal stays at full level up to 50 % and the echoes reach full level there. Time
    changes glide like tape rather than click. The plugin keeps processing until the echoes have died away.
  - **DJ EQ**: a three-band isolator with Linkwitz-Riley crossovers (24 dB/octave) at 250 Hz and 2.5 kHz. Each
    band goes from kill to +6 dB, and with all three at 0 dB it is flat.
  - **Param EQ**: three bands, each with Frequency (20 Hz–20 kHz), Gain (±18 dB) and Q (0.3–10), a shape:
    Peak (a bell), Low Shelf, High Shelf, Low Cut or High Cut (12 dB per octave, Q setting its resonance; the
    gain does nothing there) or Notch (taking its frequency away, Q setting how narrowly; the gain does nothing
    there either), and an **On** switch. All three start on, flat (at 250 Hz, 1 kHz and 4 kHz). A band switched off leaves the sound
    alone, fading out and back in over 5 ms. **Amount** (100 % to begin with) mixes what the bands put out with
    what goes in, as a Mix knob does: at 0 % the EQ leaves the sound alone.
  - **Filter**: the voice filters' types (low-pass, high-pass, band-pass, notch, comb; a row of icons on its
    card, with the list of slopes under the response) and their slopes, 6 to 24 dB, on
    the whole mix, with Cutoff, Resonance, **Key Track**, Drive and Mix. Key Track (0 to begin with) moves the
    cutoff with the newest voice's pitch, from C4 on, as a voice filter's does; once the notes end, it stays where
    the last one left it. Like the Param EQ, its display shows what it puts out behind its response.
  - **Compressor**: stereo-linked and peak-sensing, with Threshold (−60–0 dB), Ratio (1:1–20:1), a soft Knee
    (0–24 dB), Attack (1–500 ms), Release (1 ms–2 s), Makeup (0–24 dB) and Mix for parallel compression.
  - **3-Band Dynamics**: upward and downward compression in three bands, which Linkwitz-Riley crossovers (Low X,
    High X) split the mix into; at 0 % mix the bands just sum back up, so nothing combs. Each band is pulled down above its **Above** threshold (−80–0 dB, as Below) by its **Down** ratio, as a compressor does, and
    lifted below its **Below** threshold by its **Up** ratio: the gap under the threshold shrinks by the ratio, so
    a level 6 dB under Below at 2:1 comes out 3 dB under it. **Range** (0–36 dB, 12 to begin with) caps the lift,
    so silence and the noise floor aren't raised without end; a ratio of 1:1 leaves its side alone. The ratios run
    from 1:1 to ∞:1, which holds the level at the threshold (a limiter above, and below a lift right up to Below, as
    far as Range lets it); their knobs turn them up by how much of a level's distance from the threshold they take
    away, so 2:1 sits in the middle and 4:1 three quarters of the way. Then comes the
    band's **Gain**. The detector follows each band's RMS level (**RMS**, on to begin with: a 10 ms average) or its
    peaks (held between them, falling back with a 20 ms time constant, so a wave's zero crossings don't read as
    silence). An envelope follows that level at the Attack pace while it rises and the Release pace while it falls,
    and the band's gain is the curve's at the envelope's level: a note coming in out of silence takes the lift away
    within a few milliseconds, as its level climbs tens of dB in its first moments, and a sound dying away is lifted
    as its level falls at the Release pace. The lift also stops short of taking the band's peaks, which the peak
    detector catches the moment they come, more than 12 dB over Below, so a note's first moments after silence aren't
    lifted by the Range the silence had. Knee
    (both thresholds' soft knees), Attack, Release, Range, Mix and Output are shared. To begin with, each band is
    pulled down 3:1 above −20 dB and lifted 2:1 below −40 dB. **Link** edits the bands together: while it
    is on, changing one band's threshold, ratio or gain in the editor (a knob, a line, a zone, a typed value)
    sets every band's to the same value, as one undo step. Each band always plays its own settings, so turning Link
    on changes nothing until the next edit, and a modulation of one band's param moves that band alone. It
    replaces the 3-Band Comp,
    which the list of effects no longer offers: a project or preset saved with one before then loads it as a 3-Band Dynamics
    doing what it did, its crossovers, thresholds (as Above), ratios (as Down), gains, Attack, Release, Mix,
    Output and Link carried over (and the modulation aimed at them), Up at 1:1, a 6 dB knee and the detector on
    peaks; a linked one, whose bands all took the low band's threshold, ratio and gain, gives every band those.
    **OTT** in its card's right-click menu sets it after the well-known OTT ("over the top") multiband preset, as one
    undo step, fitted to what that preset does to a test signal bounced through it: crossovers at 88.3 Hz and 2.5 kHz;
    Down at 66.7:1, 66.7:1 and ∞:1 (low, mid, high) and Up at 4.17:1, as the preset has them; Above at −40.9, −37.3
    and −42.6 dB and Below at −47.9, −48.9 and −47.9 dB, each 7.1 dB under the preset's own, whose levels read that
    much over this detector's; gains of +10.3, +5.7 and +10.3 dB and Output at +5.2 dB; a 21 dB knee, Range at 36 dB
    (the preset's lift has no limit), RMS on, 100 % mix and Link off. Its curves then come out within a dB of the
    preset's in every band: a loud saw some 14 dB down, a quiet one some 10 dB up. Its attacks and releases are per
    band there and shared here: 1 ms and 30 ms follow its level jumps best (its low and mid bands release about as a
    40 ms one would, its high band as a 15 ms one).
  - **Reverb**: a feedback delay network of sixteen lines (29 to 89 ms at full size, spread evenly on a log
    scale), mixed through a Hadamard matrix, which spreads each line's sound evenly over all of them every time
    round, so the echoes thicken fast. Each line's length wavers slowly (a fraction of a millisecond, each at its
    own pace between 0.3 and 1 Hz), so the tail's resonances smear instead of ringing on as a metallic tone.
    Each side of the input goes through the pre-delay and its own all-passes, then into half the lines, and the
    two sides of the output take the lines with signs that don't match, so the tail comes out wide even from a
    mono sound. Four algorithms: **Hall** (that network, four all-passes a side); **Room** (lines under half as
    long, a darker tail and tighter wavering, with six distinct early reflections that spread with the size);
    **Plate** (lines 60 % as long, six all-passes a side for a denser start, a brighter tail, little wavering); and
    **Shimmer**, a brighter hall that feeds its own tail back into itself pitch-shifted: each side of what it puts
    out is band-passed (200 Hz up to 8 kHz at 24 dB an octave, which the damping lowers to 2 kHz, and lower still
    for a big shift up, so nothing shifts past Nyquist), pitch-shifted (two read heads crossfading as they jump back
    across a 100 ms window, the right side's a quarter of a cycle on from the left's) and fed back into the lines,
    past the all-passes (run through them again each time round, it would ring metallic). Each time round part of the tail
    climbs (or falls) by the interval, so the octaves bloom one after another as the note's own tail fades.
    **Pitch** sets the interval, −24 to +24 semitones (an octave up to begin with), and **Shimmer** how much of the
    tail's energy comes back shifted, up to 70 % (50 % on the knob to begin with; at 0 it is a plain bright hall);
    within a semitone of no shift the loop fades out, since it would only ring on unshifted. The lines decay
    faster the more comes back, so the tail as a whole still falls 60 dB in about the decay time. In the second
    after an A4 ends (2.5 s decay), its tail holds the octave and the octave above it each about as loud as the
    note. Size (the
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
    shallower again. Its polarity, **+** or **−** (+ to begin with, at the right under its display), picks whether
    the wet sound is added to the dry one or taken from it: negative turns the wet sound over, and the feedback
    with it, so every notch moves to where a peak was (between the positive ones) and Feedback keeps its sense
    either way. Switched, it fades over 10 ms rather than clicking.
  - **Flanger**: a 1–15 ms Delay that an LFO (Rate, 0–20 Hz) sweeps by Depth, with Feedback of either sign (a
    negative one gives the hollower sound) and Mix, which runs to 100 %, half wet, like the phaser's (and starts
    there). **Sync** works like the phaser's, two bars a sweep to begin with, and so does **Stereo**, under its
    display, but on to begin with: the right side a quarter cycle ahead. So does its polarity: negative puts a notch
    at 0 Hz and the others where the positive peaks were, the hollow, thin-bottomed negative flange.
  - **Chorus**: **Voices** copies of the sound (2, 3, 4 or 6; 3 to begin with), each waiting about a 1–40 ms
    **Delay** (12 ms to begin with): each voice a little shorter or longer than it, up to a fifth either way, so the
    voices don't comb alike (the first the shortest, the second the longest, the others between, interleaved so the
    voices beside each other between the sides wait unalike). An LFO (Rate, 0–20 Hz, 0.8 Hz to begin with) sweeps
    each by **Depth** (up to half of Delay either way), each voice's sweep a share of a cycle behind the one before
    (a half for two voices, a third for three, and so on), so their pitches waver against each other; and each
    drifts slowly at a pace of its own (0.19–0.41 Hz), up to a tenth of Delay at full Depth, so they never settle
    into one pattern. The voices read their lines between samples on a cubic, which keeps the highs from dulling and
    wobbling as the delays move. The bass below about 200 Hz (a one-pole's corner) goes past the voices, left as it
    is, so it stays steady and in the middle. **Spread** pans the voices across the sides at equal power, from all
    in the middle at 0 % to one side to the other at 100 % (where it starts); each side's voices read that side's
    input, so a stereo sound keeps its sides. Each voice is scaled by 1 / √voices, so more of them sound fuller rather
    than quieter: together they play about as loud as what went in. **Feedback** (either sign, 0 to begin with) sends
    the voices' mean on each side back into its line, the voices then scaled down as the phaser's and flanger's are
    so it doesn't ring louder. **Mix** takes the dry sound over to the voices alone (50 % to
    begin with). It has no Sync: its Rate is set in Hz alone. Changing Voices, a voice coming or going fades in or
    out over 10 ms and the voices' sweeps and places glide to where they go, rather than jumping; Spread moves a
    voice between the sides over 10 ms too. A host or modulation can set 5 voices, which the card doesn't offer.
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
    the 90-degree copy it needs, which keeps the mirror image (as far below the old frequency as the shift moves it
    above) 45 dB down or more from 50 Hz up; its chains are worked out for the sample rate, longer at higher ones,
    so it holds at 96 and 192 kHz too.
  - **Utility**: the level and the stereo image, in the order the sound goes through them. **Mono Bass** high-passes
    the sides (left minus right) at 12 dB an octave, so the sound below it comes out in the middle: Off at the
    knob's bottom (where a new utility starts), then 20–500 Hz; switched on or off, it fades between the sides as
    they are and high-passed over 10 ms. **Width** scales the sides against the middle, which stays as it was: 0 %
    is mono, 100 % (to begin with) leaves the sound as it is, 200 % makes the sides twice as loud. **Pan** is a
    balance: turned right it turns the left side down, all the way to silence at R100, and leaves the right as it
    is, and the other way round. **Gain** comes last, from −inf up to +12 dB, 0 dB to begin with (about two thirds of
    the way up), the knob's cube as a gain so it turns evenly through what is heard: about −6 dB at its middle.
    Its levels ramp like the other effects'.
    Its knobs come in two rows, Gain and Pan above Width and Mono Bass.

  The phaser, flanger and chorus keep their feedback from ringing louder: what they send round is scaled so their level
  across the spectrum stays where it was without it. Effect knobs can be modulated like any other. Their levels and
  mixes ramp to a new setting sample by sample, however the host splits the block. As the effects are shared by all voices, the LFOs and envelopes
  reach them through the most recently played voice. Each of the 16 places in the chain has the parameters of
  every effect type. Hosts aren't given the effects' parameters: an effect is set and automated from the editor's
  sources only, and has no remote control page.
- **Global**: **Volume**, the level of the whole output after the effects (−60 to +12 dB, −12 to begin with, room
  for chords and stacked operators; at 0 dB a full-scale operator comes out at full scale. The voices go into the
  effects at a level of their own, 12 dB down, so the effects' drive and thresholds don't depend on it, and Volume
  gives those 12 dB back. A project or preset saved by FMVibe 0.1, whose Volume didn't, loads it 12 dB lower, so
  it sounds as loud as it did. Hosts modulate it globally, and LFOs and envelopes through the newest voice, as they
  do effect knobs), FM Amount, Pitch (±48 semitones, handy as an envelope target for drums), Pan, **Bend** Range for
  MIDI pitch bend (its knob in the header, after Pitch), **Mod Wheel**, which moves what is assigned to it (nothing to begin with), **Pitch Bend**
  (±100 %, at rest in the middle), and **Voices**, the polyphony limit (1–512, default 16). Pitch Bend is the
  editor's bend wheel: it bends as the MIDI pitch bend does, added to it (by Bend Range), and the pitch bend source
  follows it. As a parameter, a host automates and modulates it like the others, globally or per note (a modulator
  per note, as Bitwig's are, bending each note its own way), and the wheel dragged edits it as a knob does, so a host
  shows its automation then (Bitwig, in its automation lane). Like the mod wheel's position, it is played rather than
  part of the patch: nothing to undo, and no modulation's target.
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
- **Sustain pedal** (CC 64, down from the middle of its travel): while it is down, a key let go leaves its note
  sounding as if still held, until the pedal comes up and the note releases. A key struck again while the pedal holds
  its note lets that note go into its release, the new note taking over. In mono, letting the sounding key go still
  returns to the most recent key held down; with none down, the pedal holds the note. A note the pedal holds counts
  as held for Legato and Glide: a new note glides from it, a mono one carries its envelopes on, and in poly legato one
  joins it. When voices run out, those the pedal holds are stolen after the released ones and before those whose
  keys are down. The keyboard strip lights only the keys held down. With MPE on, the master channel's pedal holds
  every note, and a member channel's pedal its own; with MPE off, any channel's pedal holds every note. The pedal's
  CC is also a controller like any other, which a MIDI source can follow.
- **MIDI panic**: **All Notes Off** (CC 123) lets every note go as its key coming up would (the pedal, if down,
  holding them still, as MIDI has it); **All Sound Off** (CC 120) ends every note at once, fading it out over 3 ms
  as a stolen voice does, and the effects' tails with them: every effect running fades out over 5 ms and starts
  again cleared. With MPE on, a member channel's panic reaches only its own notes (and leaves the effects alone);
  the master channel's, like any channel's with MPE off, reaches every note.
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
  parameters hosts are given accept CLAP modulation globally, the macros' knobs, the mod wheel, the note source's
  range and every modulation's depth and Amount among them (not the effects or the sources' own knobs, which hosts
  aren't given); those the voices read also per voice (not Volume, the mod wheel, the macros, the note source or
  velocity), so Bitwig's modulators (including per-note ones) can drive them directly.
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
the modulation panel or the keyboard strip makes it shorter (the plugin asks the host to resize the window). It keeps
its size as it is scaled on the monitor its window is on: in a host aware of each monitor's scaling, opened on (or
moved to) one scaled otherwise than the main one, it asks the host for the size at that scaling. While
only what keeps to its place moves (the output meter and scope and the playing count in the header, the keys, an
operator's scope in the inspector), it repaints that place alone. So it does as notes play: a source moving repaints
its card, and what its modulation moves the part of the editor that shows it (the header for its knobs, the panels'
row for an operator's, a filter's, a shaper's or an effect's params, a source's card for that source's own, and the
panels' row too while the MODS view shows); an operator's envelope repaints the inspector, and the effects' meters,
sweeps and spectra the panels' row. What may show anywhere repaints the whole of it: the note's pitch and velocity
changing, the first note or the last ending, and the host's tempo. It looks over the params for changes only when
one may have changed since it last did.

- **ROUTING / MATRIX / FX / MODS** tabs, under the header, pick what the panel below them shows: the routing graph
  or the matrix (two views of the same connections) with the inspector floating over the panel's left end, or across
  the whole panel the effects chain or every modulation. The
  choice is saved with the project, but loading a preset or Init leaves the tab shown (and the FX view's row or graph)
  as it is. They are folder tabs: the one shown is part of the panel under it, its outline
  curving into the panel's edge (ROUTING, in line with the panel's left edge, runs straight on down it), with the
  theme's accent along its top; the others are their names, a tab's shape
  showing faintly under the mouse. While the full-size shape, shaper curve or partial editor has the panels, no tab
  is shown, and clicking one closes the editor (a widened LFO card's shape stays in its card).
- **Routing panel**: the right of the panel the inspector floats over (see below); the graph shows only what is
  switched on, the matrix everything, greyed out where it is off.
  The inspector's tabs switch nodes on and off (see below); so does a node's right-click menu in the graph, and the
  x in a circle a node shows at its upper right (a box's corner) while the mouse is over it, as the effects graph's
  nodes do.
  - **Routing** (graph): circles are operators (with their wave and ratio), rounded boxes the filters (with their
    type) and squarer boxes the shapers (with their curve), each filled with the panel's color and outlined in its own
    color (each operator's, the filters' purple, the shapers' red): fully while the node is heard, as its routes are
    set (a filter or shaper while something goes into it), and at 45 % while it isn't. Its letter or name, and an
    operator's wave, take its color too, its ratio or type the dim text's. The graph sits on a radar: **OUT** is a
    disc at its right end, shading smoothly from the accent in its middle to a deeper accent at its
    edge, in a faint halo, and a bloom of the accent glowing out from it behind the graph, strongest on the core and
    feathering out further from it as light from a point does (1 / (1 + (u / 0.28)^2) of it, u the way out), steeply
    by the core and ever more gently further out, to nothing as far as it reaches. The bloom follows how loud what
    the nodes send to OUT is, before any effect: the level of the effects graph's IN (a peak falling away about 90 dB
    a second), the 12 dB the voices go into the effects with given back, from -48 dB up to full scale, which it
    rises to with a time constant of 70 ms and falls back from with one of 100 ms, so a note swells it rather
    than switching it on. From silence to the loudest, it goes from 30 % of its strength to all of it, and its reach
    from 260 px to 520 px (the outermost ring), so it spreads out as the sound swells; the Glow setting sets how strong
    it is (20 % of the accent on the core at the loudest with it at 75 %, none with it Off). It is worked out at the
    screen's own pixels and dithered, so it fades without steps: where each pixel lies is worked out only as the editor
    is zoomed, and its pixels filled again as its loudness moves by one of 64 steps. Three orbit rings go round OUT (180, 340
    and 500 px out, fainter outward) and two faint rays run from it under the inspector's lens to the lens's far
    corners, all behind the nodes, the wires and the lens. A
    halo glows around it while it sounds (feathered as the Glow setting says, a plain ring of light with it Off; it
    rises and falls as the glow behind OUT does, with time constants of 70 ms up and 100 ms down):
    an operator's as brightly as its envelope and the routes taking it to the output let it through, where modulation takes them now, so a route an LFO opens from 0 % lights it up as it
    opens; a filter's or shaper's the same way, as loud as what goes through it: the loudest of what feeds it, each
    source as loud as it is (an operator by its envelope, a filter or shaper by this in turn) as far as its route lets
    it through. A glow rises from nothing at its outer edge as a smoothstep, so it fades out without an edge or steps. The nodes that are off wait docked under OUT, in the graph's bottom right, as dashed ghosts, a circle with an
    operator's letter or a box with a filter's or shaper's name; a click switches one on, back where it was in the
    graph, and shows it in the inspector. In a new patch, each node's place is a point of the grid Ctrl+drag snaps to,
    mirrored about OUT's row: the operators on the left (F and E in the second column at the top and bottom, C and B in
    the fourth a row in from them, D and A in the seventh at the top and bottom), then the shapers (S2 above OUT's row,
    S1 below it) and nearest OUT the filters (F2 at the top, F1 at the bottom). However it is switched on (its ghost, its tab, the matrix), a node whose
    place another node shown now covers comes back clear of it: pushed out of the way, or, with no room there, at
    the free place nearest it (in the same undo step as the switch).
    - Drag from a node's **edge** (a band along its outline, which glows in the node's color under the mouse)
      onto an operator to modulate it, back onto itself for feedback, onto a filter or shaper to send it there, or
      onto **OUT**; from further inside, a drag moves the node, and a click on the edge only picks the node.
      Filters' and shapers' edges work the same way: onto an operator to modulate it, onto a filter or a shaper,
      back onto itself for feedback (starting at 25 %, its loop drawn on its top edge), or onto OUT. Every node
      can feed every other one, and itself. While the drag (or a route's arrowhead being moved) is held back over
      the node it comes from, its line loops round in a small circle over the node's top and back into it, an
      arrow showing the loop letting go there makes (standing on the ring the node is ringed with, selected or not); only once the mouse has been well clear of the node (16 px past its
      outline), so the loop doesn't flash as a drag leaves the edge. Operators, filters and
      shapers go out alike: each has its line to OUT while it is routed there, whether anything goes into it or not.
      A node that already goes out is moved through a filter or shaper it is connected to rather than doubled, its
      output's modulation going along onto the send, and that goes out in its place unless its sound gets there already (through filter 2, say); so does a filter or
      shaper a node's arrow to OUT is moved onto. Each wire takes the color of the
      node it comes from (an operator's own, the filters' purple, the shapers' red), as its line to OUT does.
    - A node's line to OUT runs straight toward OUT's middle, in the node's color, and ends in an arrowhead on its
      edge, like a wire's on the node it feeds, with its percentage label halfway along (beside the line, on its
      left, when the node sits too close to OUT for it). OUT is ringed in the accent while a route held over it would
      go there. Drag an arrowhead onto another node to move the route there, or onto OUT to send its node out
      instead; the line to OUT's onto a node the same way. The route keeps its amount and its modulation, and
      follows the mouse until it is let go; it can't go where its node has a route already, or where a node's edge
      couldn't take it. Let go of off the nodes and OUT (where it turns faint), the route goes, with its
      modulation, as its right-click menu's Disconnect does. The feedback loop above an operator or a shaper has no
      arrowhead.
    - A wire's ends drag from a little way along it too: under the mouse, a faint circle rings its arrowhead, or
      its start, with a dot where it leaves its node.
    - A wire's start, where it leaves its node, drags the other way: let go of on
      another node, the route comes from there instead, with its amount, its mode (while it still goes into an
      operator) and its modulation, as one undo step; a line to OUT's start onto another node sends that node out in
      its place. It can't come from a node that has that route already. Let go of off the nodes, the route goes.
    - Under the mouse, a wire's or OUT label opens into a pill, its depth staying where it was: on its right an ×
      that removes the route with its modulation (one undo step, as Disconnect), and on its left, for a route into
      an operator (its feedback aside), its mode's glyph (φ for Phase, a straight line for Linear FM, a steepening
      curve for Exp FM, a ringed cross for Ring) and a chevron. A click there opens the list of modes, and the wheel
      steps through them. The pill stays open while the mouse is on it, its list is open or its depth is dragged.
      Resting, a label shows a mode other than Phase inside it, at its left. The right-click menus of the wire, its
      label and its arrowhead list the modes too, the one in force checked. A route moved onto another operator
      keeps its mode; moved elsewhere or removed, it is Phase again when it comes back.
    - Drag a wire's percentage label up or down to set the depth; double-click it to remove the wire, and
      Ctrl+Click it (or pick Edit value… from its right-click menu) to type a depth. A wire, or
      a node's line to OUT, turned down to 0 % stays, so modulation can still be dropped on it; only removing it
      (double-click, or Disconnect in its right-click menu) takes it away, together with its modulation. Dropping
      an LFO or envelope on a matrix cell shows that route's wire too, even at 0 %.
    - A wire between two nodes bows to its left as it starts, so two running opposite ways between the same nodes
      don't overlap. To keep wires apart where they would, drag one sideways anywhere along it (it lights up under
      the mouse, apart from its label and arrowhead): its middle follows the mouse, out to either side or straight,
      its label and arrowhead going along. A label that would sit over a node (one placed where the wire's middle
      runs) or over another label slides along its wire, either way, as little as clears every node and the labels
      placed before it (the feedback loops' first, then the wires' in the graph's order, then the lines' to OUT), and
      goes back to the middle once that is clear; a line's label to OUT does the same. Where no place along its wire
      clears the other labels too, it clears the nodes alone. A feedback loop's label sits on the loop's right, or,
      where that would meet another node, another loop's label or where a wire's label rests, on its left, above it
      or at an upper corner, inside the panel. A wire that would spill out of the routing panel bows only as far as
      keeps it and its label inside, whether it is bent or its nodes are moved toward the edge, and bows as it was set again
      once there is room. A double-click on it, or Reset Bend in its right-click menu, brings back
      the bow it started with. A node's line to OUT runs straight to begin with, and bends the same way, its label
      and arrowhead going along; a double-click or Reset Bend straightens it again. How each wire bends is saved with the patch, like the node places, and each drag is
      an undo step; a wire that is removed or moved to another node starts bowing as a new one does.
    - Drag nodes to rearrange the graph (saved with the patch). Click one to edit it. The node the inspector shows
      is filled solid with its color, like its tab's plate, its letter, wave, ratio or type in the panel's color, and
      ringed in it at 80 % a little way out; a dotted line in its color, the lens's leader, runs level from its left
      side across to the inspector's lens; a switched-off one shown
      there has its ghost filled the same way. A node dragged onto another
      stops short of it, held 10 px off its outline and sliding round it, and goes on once the mouse is past. With
      Ctrl held, a node dragged goes onto the nearest point of a coarse grid (12 across, 5 down, the cells' centers
      over the area nodes may sit in) where it fits clear of the others, the next free one if that is taken; pressed
      or let go of mid-drag, Ctrl snaps or frees it at once. While Ctrl is held over the graph, a faint + marks each
      of the grid's points, and the one a dragged node sits on is lit. The marks show and go as Ctrl is pressed and
      let go of, also while the host has the keyboard (for a press, with the mouse over the editor while a window of
      the plugin's process is in front, so Ctrl held in another application shows nothing).
  - **Matrix** (grid): each row is a source (an operator, filter or shaper), and its cells say how much of it goes
    to each column: the operators, F1 and F2, S1 and S2, and OUT. It always lists all ten nodes and fills the
    panel's height, as far from its top as from its bottom; every cell is a route; the row and
    column of a node that is off are greyed out, its routes kept as they are. The diagonal is each node's feedback,
    outlined in its color; toggled on, it starts at 25 %. A route into an operator that isn't Phase shows its mode's
    glyph in its cell's top left corner, and the cell's right-click menu lists the modes. Drag cells
    vertically; double-click toggles a cell on or off; Ctrl+Click types an amount. Dragging a cell, or toggling it
    on, switches on the nodes it joins. A row's or column's name works like the node's tab in the
    inspector: clicking it shows that node there, switching it on if it is off, and the names of the node shown
    sit on a plate, like its tab, where a click switches it off, or on again.

  - **FX**: takes the place of both the routing panel and the inspector. It shows a card per effect, left to right
    in processing order, each in its type's color (shaper red, delay blue, DJ EQ amber, Param EQ orange, filter
    purple, compressor green, 3-band dynamics teal, reverb indigo, phaser pink, flanger lime, chorus periwinkle,
    frequency shifter cyan, phase disperser slate, widener fuchsia, amp tweed gold, utility gray) and
    with a title row laid out as every card's (the source cards' too): its icon and its name in capitals at the left, then a
    chevron that folds it, its power switch and an × to remove it at the right; and under it a live display, which
    draws the effect as modulation takes its params now (the newest voice's, or the macros' while nothing plays); the
    Param EQ's band handles and the 3-band dynamics' threshold lines stay where they are set, as the filter's handle
    does:
    - the shaper's transfer curve; a Custom curve shows its own points and bends instead, to drag right there
      (double-click adds or deletes a point, Ctrl snaps to the grid), with what the drive, output and mix make of
      it faintly behind and the point under the mouse in numbers; its edit button (a pencil) opens the big editor;
    - the delay's echoes on a timeline, left above the line and right below it;
    - the EQs' and the filter's responses; behind the Param EQ's curve, what it puts out, measured (a 2048-point
      FFT in 96 bands from 20 Hz to 20 kHz, 0 dB at the top, falling away at 55 dB a second), which it only
      measures while the editor is open; the Param EQ's and the filter's sit on a grid, dashed every 12 dB with
      0 dB solid, and dashed at 100 Hz, 1 kHz and 10 kHz;
    - the compressor's curve (input level across, output up) with a gain reduction meter;
    - the 3-band dynamics' levels: a bar a band, the high band on top, from −80 dB at the left to 0 dB at the
      right, with the band's name at its left and its gain at its right. Down the bar's middle runs a meter half
      its height: in the card's color up to what comes out, then the gain, amber where the band takes some off
      (up to what went in) and in the accent where it adds some; a white tick across the bar marks what goes in.
      Both ends are held meters falling alike, about 26 dB a second: what goes in, and that through the band's
      dynamics. The gain at the bar's right is the distance between them. Both show what the dynamics do: the
      band's Gain, which comes after them, is left out, so amber is the band pulled down and the accent the band
      lifted. So as a band goes quiet, and its lift comes for what is left, the meter falls whole rather than its
      far end shooting up.
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
      Link sit at the right end of the line over the shared knobs, the title row holding only what every card's
      does. The
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
    - the chorus's voices: a lane each across a cycle of its sweep, its delay (up for longer) about its own place
      along the delays, the dashed middle line Delay, as far either way as Depth swings it, each lane a share of the
      cycle behind the one before and fainter, the first brightest (their slow drifts aren't drawn); while the audio
      runs, a dot on each lane where its sweep is now. Over it, how many voices and the Rate;
    - the phase disperser's delay across frequency, peaking at Freq, with what it puts out, measured, behind it
      like the Param EQ's;
    - the frequency shifter's move: a 220 Hz tone's partials, and where it takes them on each side, under the
      shift in Hz and the range it is a share of;
    - the widener's stereo field: left to right across, frequency up (20 Hz to 20 kHz), the dry sound a line
      down the middle and the sides it adds spreading out above the high-pass (a dashed line), as far as Width
      and Mix take them, wavering with the sweep while the audio runs; what Width leaves in the middle thickens
      the line. Above it, the high-pass and the taps' delays;
    - the utility's stereo field, as the widener's: left to right across, frequency up, the sound a band about its
      middle, which Pan moves across and Width widens (a quarter of the field either way at 100 %), narrowing into
      its middle below Mono Bass's line (dashed, its corner over it: `mono < 120 Hz`), and fading as Gain goes down
      below 0 dB, gone at −inf. At its left, a strip shows Gain, a tick across it at 0 dB, and above it Gain and Pan
      (`-6.0 dB · L30`). Each knob has a handle there: Gain's a cap across the strip, which drags up and down; Pan's a dot at the top of the middle
      line, which drags across; Width's a dot on the band's edge, on the side with more room, which drags out and
      in; and Mono Bass's a dot at the right end of its line (at the bottom, faint, while it is Off), which drags up
      and down, back to Off at the bottom. Each drag is one undo step, and a double-click puts a handle back where
      it starts;
    - the amp's two displays, side by side. The amp's: the tone's frequency response (20 Hz to 20 kHz, +12 to
      −30 dB), its tone stack and Presence through the cabinet and mic while the cabinet is on (solid, filled) over
      the tone stack alone (faint, dashed), or the tone stack alone with the cabinet off; at its right, a drive meter
      rising with how far its stages saturate now (in dB, 36 at the top), and the figure at its top left. The
      cabinet's: the speaker seen from the side, the baffle and the cone behind it, and the mic in front of it, which
      drags up across the cone for Position and away to the right for Distance, in one undo step (a double-click
      puts it back where it starts); set back far enough, faint arcs of the room it hears; the mic and its distance
      at its top left.

    The delay's switches under its display are icons across the card, named on hover: a metronome for Sync, two
    chain links for Link (the right side follows the left) and a ball between two paddles for Ping-Pong. The
    shaper's Link (which ties its Output knob to the drive) is the same chain, and so is the 3-band dynamics' (at
    the right end of its line over the shared knobs), centered on the card under its display; for a Custom curve the shaper's Symmetric shows left of it, a curve
    rising to level off, its left half the right one turned over, the two centered together. The shaper's curve's list takes the whole row above the display. Every effect with a Sync
    has that metronome: the phaser and flanger at the left under their displays (a button of its own, STEREO
    another beside it, and their polarity's + | − at the right), over their Rate knob, which, synced, picks a note
    value and shows it as its label, as the delay's time knobs do. The chorus has its voices, 2 | 3 | 4 | 6, in the
    middle under its display, and its knobs in two rows, Rate, Depth and Delay above Feedback, Spread and Mix. The filter's slope is a list under its display, like a voice filter's (see the inspector) and a split's. The filter's
    knobs come in two rows, Cutoff, Reso and Key Track above Drive and Mix, and so do the
    frequency shifter's, Range, Shift and Spread above Feedback and Mix.

    The amp's card is 1.85 places wide, the amp on the left and the cabinet on the right with a faint line between, in
    the order the sound goes through them. The amp's side: the model's list, its display, STEREO under it, and Gain,
    Volume and Presence over Bass, Mid and Treble. The cabinet's side: its
    power symbol, then the cabinets' list, its display, the mic's three (DYN, CND, RIB) under it, and Position and
    Distance over Output and Mix. With the cabinet off, its display, the mic's switch and Position and Distance dim,
    though they still turn and take modulation.

    Each effect has an icon, at the left of its card's title and in the list of effects. On the card, the power
    switch in the title row turns the effect off (the icon turns grey with the card, under the mouse too, and the
    effect costs nothing) or on again, in its color; a click on the icon does the same, and dragged, the icon moves
    the card like the title. Folded, the strip keeps the icon, a grip, the power switch and the ×. **+ EFFECT** (a
    drawn plus, the word and a chevron, neutral until the mouse is on it or its list is open, then in the theme's
    accent, as + SOURCE is) at the panel's top left opens a list of the effect types (beside it, how many of the 32
    places are taken, as in 3/32), and so does a dashed card with a plus after the last effect, at the mouse (moved
    back inside the editor where it would reach past an edge). The list groups the effects by kind in two columns,
    a little apart, each group under its name in capitals on a darker strip: DRIVE (Shaper, Amp), EQ & FILTER (DJ EQ, Param EQ, Filter) and DYNAMICS
    (Compressor, 3-Band Dynamics) down the left one, SPACE (Delay, Reverb), MODULATION (Phaser, Flanger, Chorus, Freq Shifter)
    and STEREO & PHASE (Widener, Phase Disp, Utility) down the right one. The
    one picked goes at the end of the chain, which scrolls to show it, and so does a copy. An arrow goes before each
    card, the first too, and turns into a plus under the mouse: a click opens the list over the cards there, and the
    one picked goes in there. While there are effects, a hint at the panel's top right tells how to work the cards
    ("drag a title to reorder"). The dashed card takes a
    card place at the chain's end while there is room for another effect. Six cards fit across, the Param EQ's
    taking one and a half places for a wider spectrum (the 3-band dynamics' too, and the amp's 1.85); with more,
    a scrollbar under them (drag its thumb or click its track; drawn thinner while the mouse is away from it) or the
    mouse wheel over the panel scrolls the chain smoothly, cutting off the cards at the ends, and so does a drag with
    the middle button held (see Everywhere). Drag a card by its
    title to move it along the chain (held at either end of the row, it scrolls the chain along that way; while it
    is held, only its outline shows where it would land, and a faint image of it follows the mouse anywhere, over the
    other panels too; the cards glide aside to make room as soon as it covers a fifth of one, from either side, and
    a folded container once its left edge is past the container's middle. Once the mouse has been over a split
    container, where it lands goes by the mouse, as the row shows: across from the container's controls it stays as
    it was, so an effect on a band can be carried over them to another band's tab, which lights in that band's color
    and, let go of there, puts it at that band's end, the container still showing the band it showed; over the
    container's band it goes along that band; out of either side of the container it lands right beside it (left of
    its controls is before it, so a container first in the row takes a card before it with the mouse past its
    controls' left edge); and past the next card once the mouse is beyond that card's middle. Let go of, they glide
    on into place, and a click while they glide puts them in place at once). Hold Ctrl as a card or container starts
    to move to carry a copy of it instead (with its settings, but not its modulation; a container with its join and a
    copy of each effect on its bands, its crossovers, slope, band levels and solos): the original stays where it is,
    the copy's place opens right after it, and the copy lands where it is let go of, the copy and its place one undo
    step. A drag called off leaves nothing. With too few free slots or splits it says so, and moves the card instead;
    so does a split left open at the line's end, which the line has room for only one of),
    and right-click the title (or the icon) to duplicate the effect (with its settings), reset its settings,
    set a 3-band dynamics as OTT, pick an amp's oversampling (**Oversampling 1x** to **8x**, the one in force
    checked), move it to another band of its split (**Move to Low**, **Move to Mid**, **Move to
    High**, on a card on a split's band: the split's other bands, the effect going to that band's end, one undo step),
    or remove it. The chevron left of a card's × folds it to a narrow strip, as wide as the icon's place at the
    title's left, so the icon stays where it was: the icon, which still switches the effect and drags the card,
    a grip (six dots) that drags it, its ×, then its name down the strip, which unfolds it. The cards after it close up; folds are saved with the
    patch but not undone. On the Param EQ's display,
    drag a band's numbered handle for frequency and gain, Alt-drag it up or down (it stays where it is) or scroll
    on it for Q, and double-click it to flatten it; a cut's handle sets its resonance instead of a gain, and a
    double-click takes the resonance away. A notch's handle sits on the 0 dB line and only moves across (Alt-drag
    or scroll for its width), and a double-click sets its Q back to 1. A band switched off shows a faint handle;
    pressing it switches the band on (and it can be dragged on from there).
    Under the display, a chip for each band: its power symbol and its number (in the band's color while it plays),
    which together switch the band on and off, and its shape's icon, which opens the list of shapes over the
    display. Either part of a chip, or a band's handle, picks the band the row of knobs under the chips (Freq, Gain
    and Q, as large as a one-row card's) sets; its chip and handle are ringed in its color. Amount, the row's fourth knob, is the whole EQ's. The band picked isn't saved: a card shows its first band to begin with.

    A **split** shows in the row as a container card holding its bands, its grey outline a little outside the cards
    in it, so they line up with the row's, and as much more room before and after it as the outline stands out, so
    the arrows on either side are as far from its outline as from any card. At its left, on grey, a card place wide: its title (the split's icon and
    name, SPLIT 1, then the **2 | 3** switch, which splits it in two bands or three: its tooltip says which, and the
    wheel switches it too), which drags the split with its bands and its join along the line (between the main line's
    cards only), the chevron left of its × folding the container to a strip as an effect card folds, and its ×
    taking the split out. With nothing on its bands it just goes; with effects there it asks first ("Remove the
    split?"): **Keep** puts them on the main line in its place, the high band's first, **Delete** removes them with
    the split and their modulation, each one undo step, and Cancel, or a click around the dialog, leaves it. (The × of
    its folded strip, of its node and its join's in the graph, and its node's right-click menu ask the same.) Under the
    title, the split's controls: **LOW | HIGH** tabs (**LOW | MID | HIGH** in three bands), as an operator page's
    tabs, the one shown underlined in its band's color (the low band blue, the middle one sea green, the high band
    amber, each theme with band colors of its own; the wheel steps along them too), a hidden band's tab saying how
    many effects it holds after its name and a soloed band's showing an S in its color in its dot's place; the bands'
    responses with a handle on each crossover to drag across, each band's curve lowered as far as its level's knob
    is turned down (flat at -60 dB), the shown band's filled, and a click between two crossovers (or a crossover and
    the display's edge) showing the band there, as its tab does; under them, at the right, the slope's list, as a filter's is, 6, 12 or 24 dB (18 shows faintly and can't be picked; in three bands 6 does
    too). Switching between two and three bands keeps the slope, but for 6 dB, which goes to 12 as it splits in three,
    in the same undo step. In two bands, three knobs: the low
    band's level, the Crossover and the high band's level (in dB, 0 dB, full, at the top, down to -60 dB, silence).
    In three, the display is shorter, and a row of two smaller knobs, Low X and High X, the crossovers, goes under
    it, over the low, the middle and the high band's levels. Under each band's level an **S** solos it, each band's on its
    own (a click on the lit one unsolos it; with two of three soloed, only the third is left out), lit in its band's color. Switched to
    three bands, as one undo step, an empty middle band opens between the two, shown, its crossover at 2500 Hz, or two
    octaves above the low one where that is higher (as high as it goes), so a new split's 250 Hz gives 250 and
    2500 Hz, and its level at the join 0 dB. Switched back to two, an empty middle band
    just goes; one holding effects asks first ("Change to two bands?"): **Onto Low** puts them at the low band's end,
    **Onto High** at the high band's start, **Remove** removes them with their modulation, each one undo step, and
    Cancel, or a click around the dialog, leaves it in three. To the right of the controls, the
    cards of the band shown, the arrows into them in the band's color (faint, cards and arrows, while another band
    is soloed), then
    a narrow dashed card (ADD TO LOW, ADD TO MID or ADD TO HIGH, in capitals as ADD EFFECT) that opens the list of effects at the mouse (effects only: a band holds no split), the one
    picked going at the end of that band, with no arrow before it. The arrows inside the container add an effect to
    its band there (the first, after its controls, at the band's start). The main line's arrows, the dashed card at the row's end and
    + EFFECT offer **2-band split** and **3-band split** as one more group, SPLIT, the one over the other under
    DYNAMICS, which go in there (at the
    line's end from the last two) with their join, their bands empty and their low band shown. A card dragged over the band a container shows
    lands on that band, and let go of on a container's tab it goes to the end of that band (the tab lit while it is
    over it; the band shown stays as it is); a card dragged out of a
    band goes onto the main line. A copy of a card on a band goes at the end of that band. The band each split shows
    and which containers are folded are saved with the patch, but not undone.

    Left of that switch, a bin removes every effect, in the chain and in the graph (with every split and join), as one
    undo step: while there is any, it asks first ("Really remove all FX?"); with none, it is faint and does nothing.

    The switch at the panel's top right (cards in a row, or a node forking and meeting again) shows the effects as
    a **graph** instead: at the right the card of the node picked, as it would show in the row but without its ×
    (the node's own × removes it: see below), an effect card's power switch taking the ×'s corner, and the graph at the
    left in the room the card leaves: beside a card one place wide the graph reaches further right, and a wide card
    (the Param EQ's, the 3-band dynamics') takes some of its room. The graph scrolls as far as it needs beside the
    widest card, so picking a node never moves it: scrolled to its end beside a narrow card, a little room shows
    after OUT. Its hint at the top right reads "click node: edit · drag
    node: move · click wire: insert". The graph's nodes sit on a fixed grid, a column each, and
    wire themselves: wires, grey along the main line, run left to right from the input (IN, a
    box at the left) to the output (OUT, a box after the last column). A wire from one row to another runs level as
    far as it can and bends S-shaped only within the gap between two columns' nodes, so it never crosses a node: out
    of a split's band right as it leaves the split, into a join (or OUT) just before it. Each effect is a rounded box in its color, as a voice
    filter's node is in the routing graph, its icon in it and its name under it, on the main line across the middle. A **+** in a circle marks where a node can go next; a click on it
    opens the list of effects beside it, and the effect picked goes there. The main line's list also offers **2-band
    split** and **3-band split**, in its SPLIT group: a split, a box its high band leaves from the top of
    its right side, its low band from the bottom and a middle band from the middle, always comes with its join, which
    adds the bands back together a column after the longest band's last + (each band's + has a column of its own); the line goes on from the join. The bands run
    between them, the high one along the row above the main line, the low one along the row below and a middle one
    along the main line's row, named beside the split, each band's wires in its color (the high band amber, the
    middle one sea green, the low band blue) all the way into the join. Splits and joins are boxes as large as
    each other, their upper halves in the high band's color and their lower halves in the low band's (in three
    bands, their thirds, the middle band's between), parted by slightly slanted lines (a join's the split's mirror): a split's colors come in from grey at its left, where the
    main line comes in, and a join's go back out to grey at its right, where the main line goes on. While sound plays, each node glows round it and each
    wire along it as loud as what comes out of it (IN, an effect, a split's band, a join), as the operators do in the
    routing graph: from -48 dB up to full scale, falling away about 90 dB a second once it stops, and shown rising and
    falling as the routing graph's glows do (time constants of 70 ms up and 100 ms down). An empty band curves out to a + on
    its row, a column on from the split, and on into the join, and a click on that + puts an effect on the band (its
    list offers effects only). Each + is ringed in the color of the wire it is on: grey on the main line, a band's
    color on a band. With the mouse
    anywhere along a wire between two nodes on the main line, a + shows at its middle, and a click on that + opens
    the list too. A split's band shows its + at its end always: in its next place, a column on from its last node on
    its row (on an empty band, a column on from the split), the join a column further on. Every + in the graph is faint until the mouse is on it
    (or its list is open), and none shows while a node is dragged, the wires running straight through. The list a wire's + opens puts the effect picked
    in there: on the main line, the list also offers the split, which goes in with its join right after it and
    the bands empty between them; on a band, effects only, so a joined band takes them by the wire into its join. A split's
    node and its join's both show the split's card: the same controls as its container in the row (see above), on
    grey, a card place wide, its tabs picking the band its display fills as the row's do (and so the band its
    container shows). While a band is soloed, the split's and the join's nodes show an S in a circle at their top
    left, in the soloed band's color (the accent with more than one soloed), and the bands not soloed (their effects'
    nodes, their wires and their names) are drawn faint, as the join doesn't let them through. Under the mouse, a node shows an x in a circle at its upper
    right, which removes it as the x on its card does, and an effect's node a power symbol in a circle at its upper
    left (in the effect's color while it is on), which switches the effect on and off as its card's power switch
    does, an undo step each. A node dragged onto
    another place moves there, one undo step: while it is held, the others glide aside to open the place it would
    land in, outlined dotted, which moves only once it is over another gap between two nodes (an effect's anywhere on
    the main line or a band, a split's only on the main line; the line's end reaching on past its + along the wire
    to OUT) or the mouse is anywhere on a wire, its arrowhead too, which takes it where that wire goes (into the node
    it ends at, or on to a +, a join or OUT the end of its line or band), and stays where it is
    while it passes over a node or away from the line. The dotted place goes where it would land at once, and the
    wires run to it there as to a node, never to the node being carried; let go of, the node takes that place, and
    the others glide on into theirs. A split or a join
    carries the split with its bands' effects and its join along. With Ctrl held as it starts to move, a node carries
    a copy instead, as a card in the row does (a split's or a join's: the split with its join and copies of its bands'
    effects), its place opening right after the original and the copy shown in the card at the right. Held at the graph's left or right edge, it scrolls
    that way. The x on a card removes its node: an effect closes up the
    line or band it was on, a split puts its bands' effects on the main line in its place (the high band's first),
    and so does a join, which goes with its split. + EFFECT puts the new
    effect after the node picked: after an effect on its line or band, after a join on the main line, and at the end
    of a split's shortest band (the highest of those as short); with nothing picked, at the line's end. A copy goes right after its original. Wider than its box, the graph
    has a scrollbar under it (its thumb dragged, or the track clicked to jump there), and scrolls sideways under the
    wheel too or with the middle button held; it shows a node just added. Switching to the
    graph or back changes nothing but the view: both show the same line, splits and all. A click on a node shows it in the card at the right;
    right-click an effect's node for its card's menu, a split's or a join's to remove it. The node picked is saved
    with the patch; the effects' count beside + EFFECT and the target menu follow the line's order.
  - **MODS**: takes the place of both too: every modulation as a row of a table, by source (the LFOs and envelopes
    in the order their cards show, then the macros, the note source, the mod wheel, velocity and the pitch bend), the
    first of a
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
      an entry under the mouse. The source's lists the sources by category (LFOs, the Random and Curve sources among them, Envelopes,
      Note & MIDI for the MIDI sources, Macros, and Keyboard:
      Note, Velocity, Mod Wheel and Pitch Bend); picking one moves the modulation there, into a free slot, with its depth,
      curve, Amount, aux and bypass, as one undo step; it then swings as its new source does. What modulates its depth
      follows it to its new slot, or, moved to a macro, the note source, the mod wheel, velocity or the pitch bend (whose depths
      can't be modulated), goes. A source that modulates the
      target already, or has no slot free, is listed faintly, saying why, and can't be picked. The target's lists
      every target by category: Global (but the mod wheel, which is played rather than set); Operators, each with
      its params in two columns, under PITCH, LEVEL, PHASE (with Warp Amount) and UNISON, and beside them ENVELOPE
      and NOISE;
      the Matrix, by the node a route comes from (into each other node it can feed, out, and its feedback last); Filters; Shapers; Effects,
      only those in the line, in its order, each with the knobs of its type, then each split's crossover (its two
      in three bands) and each join's High and Low levels (and Mid); and Modulation, each LFO's and
      envelope's own knobs and the depths of its modulations; then Macros, the macros' knobs by their names (the row's
  own macro's, and one that would make a loop, listed faintly, saying why). Picking one moves the modulation there, keeping the
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
    fourth); the ones a lower count hides keep what was drawn. The random button (a die), left of the reset
    button, gives all 64 partials a random level under a pink slope (partial *k* at most 1/√*k*, falling 3 dB an
    octave) and a random phase, in one undo step. The reset button (a circular arrow) goes back to a
    sine, leaving the count, the done button (a check mark), or a tab, closes it, and Custom from the list or the
    edit button (a pencil) on the operator's summary line opens it again. A stroke is one undo step.
  - **Shape editor**: adding a Curve source, a click anywhere on a Curve source's display (which shows an
    edit button, a pencil, in its corner), or the expand button in its title row, left of the fold button, widens its card to two card places, the cards after it moving along and the row scrolling to show it
    whole, to edit the shape right there while the panels above stay in use: the inspector's knobs, the routes and
    the other views can be turned and switched as the shape changes. The widened card keeps the source's own controls where a normal card has them,
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
    another Curve source (faint until a shape is copied; for now Curve sources' shapes only). Under them, **Points | Steps** picks what a
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
    rings for the points it mirrors, the points left of the middle are hidden, and a click there does nothing (no
    point added, no step drawn; a step held across the middle stays where it last was on the right). The editor has
    its own **SYM** switch, left of Points | Steps (in the title row for a shaper effect, under the plot for a voice
    shaper), as the card and the shaper's page do. Switching Symmetric on gives the curve a point in the middle if it has none,
    which then stays there: it only moves up and down, and can't be deleted (a step drawn across it keeps it, at the step's height). The note source's curve opens over the routing panel from its display in the
    keyboard strip (a second click closes it): the notes from Low to High across, the Cs among them named under
    the plot, and what the source puts out up, from nothing at the bottom to all of it at the top, with a dot
    where the newest voice is while notes play. A dashed line shows, for the range set, what would take a cutoff
    at 0 by 100 % onto the note. **Low** and **High** are knobs side by side in its title row, dragged in whole
    notes (or typed, as `C4`, `F#2 -20` or a MIDI note number); its ends stay at the ends too, and the reset button
    brings back every MIDI note and the curve along the dashed line. Velocity's curve opens over the routing panel
    the same way, from its display (see Velocity, under the keyboard strip).
- **Header**: two rows. The **title bar** holds the name at its left, the preset bar in its middle, and undo and
  redo, Init, Load, Save, Save As and the settings cog at its right (see Presets and Settings, below). In a host that
  stands in for a transport and lets the editor set its tempo, the browser demo and `fmvibe-uihost`, a **tempo**
  field sits between the name and the preset bar: a metronome, then the tempo in BPM (20–999, 140 to begin with),
  which every synced LFO, delay and sweep follows. Dragged up or down it moves a BPM every 2 px, onto whole ones (a
  tenth of one with Shift); the wheel steps it a BPM a notch (a tenth with Shift); a double-click or Ctrl+Click types
  one, with or without `BPM` after it, held to the range. It is the host's, not the patch's: presets and undo leave
  it alone. A plugin host's tempo is its own, so in a DAW the title bar shows none. Under it, the
  **global strip** spreads across the editor, from the output scope under the name to the voice count at its right
  end. First the output scope:
  the output after Volume (its two sides' mean) over two periods of the newest note's pitch (kept once the note has
  ended, as the effects' key tracking keeps it; a high note's span holds a whole multiple of two periods, at least half
  a millisecond). Its spans follow one another at that pitch, so a sound that repeats with the note stands still
  whatever its wave, and a new note starts them over from where it starts; silence shows as a flat line. A low note's
  spans, coming more than 15 ms apart, blend one into the next over the time between them, so the trace moves smoothly
  a span behind rather than jumping a span at a time. Its points
  fall between samples, and are taken band-limited from the 16 samples around them, so a wave dense with high
  partials looks the same wherever the samples fall in it. The trace is
  drawn to a level that follows its peaks, up at once and down to a third in about 300 ms, so a level moving from
  span to span doesn't make it jump; that level sits at the display's edges, lifted by no more than 24 dB, so the
  shape shows at any volume while a fading tail shrinks away. The audio thread captures only while the editor is open. Then the output meter, left of Volume:
  a bar for each side showing its peak, from −48 dB at the bottom,
  in the theme's color up to full scale and red past it, falling 24 dB a second (also once the plugin stops
  processing); the strip above full scale lights up red for a second and a half after the output went over. Then
  the global knobs (Volume, FM Amount, Pitch, Bend and Pan, close together); the play cluster between two dividers (Poly/Mono over Legato, which works in both, then
  a Glide knob like the global ones, its Always switch beside its label, each switch explaining itself on hover);
  Voices (it drags on a log scale, so 4 and 512 are both easy to reach), right beside how many voices are playing.
- **Undo and redo**: the curved arrows left of the preset buttons step back and forward through the patch's
  edits, up to 100 steps; hovering one says what it would undo. A drag, a run of wheel notches on one knob, a typed
  value, adding or removing an effect or source (with its modulation), moving nodes or cards and bending a wire are
  each one step. The history lives in the plugin, so it outlasts closing the editor, and goes when the host loads a
  saved state, at Init and when a preset loads: undo never brings back the patch a preset replaced. **Ctrl+Z** undoes, and **Ctrl+Y** or **Ctrl+Shift+Z** redoes, while the mouse is over
  the editor and its window is the active one: a keyboard hook catches them before the host does, so the host's
  own shortcuts keep working everywhere else.
- **Inspector** (left): a lens, a card floating over the routing panel's left end a little in from its edges, on a
  soft shadow, the graph's backdrop faintly through it, outlined in the color of the node it shows at 35 % (in Spectrum
  and Porcelain, as their cards, on its shadow alone, without an outline); while
  the graph shows that node, a notch on the card's right side meets the leader from it. Tabs A–F, F1 and F2, and S1
  and S2, in three groups with a hairline between two, pick what it shows; tabs of switched-off nodes are
  dimmed, and clicking one switches its operator, filter or shaper on; the names of switched-on ones take a little
  of their node's color. A dot in a tab's top right corner says the
  node's own sound goes out: its Out is up and it is switched on (a filter's or shaper's only while something goes
  into it). Clicking the
  selected tab switches it off, or on again (its
  tooltip says which). Under the tabs, the page's title: the node's letter, at the heading's size in its color,
  then a summary line that sums the node up in short: an operator's wave, ratio, unison copies (×4) and where it
  goes ("A Saw · 1.20 · ×4 · to F1"), or a filter's type and slope, or a shaper's curve, and what feeds them, where
  they go and the operators they modulate; the wave, type or curve in full text, the rest dimmed. The power switch
  next to it does the same as clicking the selected tab. While the node is off, its page is dimmed below the summary
  line; its knobs still work.
  - Operators: the operator's sound in one block. At its left, as tall as two rows, the Ratio readout; at its right,
    the waves as one segmented row of buttons, the common waves, then **…**, which lists Pulse, Half Sine, Abs Sine,
    Alt Sine, Organ, Vowel and Custom (picked, one of these shows on that button), and under them the pitch, detune,
    phase and pan knobs, their labels on the Ratio readout's line. The Ratio readout is four digits of a 7-segment
    display (88.88, the tens dark below 10),
    each its own handle. A digit dragged up or down steps by its own place (tens 10, ones 1, tenths 0.1, hundredths
    0.01), a step every 6 px (24 with Shift), carrying into the next digit and held to 0.50 to 64.00; the wheel steps
    the digit under the mouse, and a double-click resets it and the digits below it to those of 1.00 (on the ones, 23.47 becomes 21.00). The digit under the
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
    an edit button (a pencil, the same as a Curve source's and a drawn shaper curve's), left of the keyboard, opens
    the operator's partial editor (lit while it is open; a second click closes it, as does picking another operator, filter or shaper in the
    inspector). Under the block, past a line across the page, a well: tabs along its top edge pick what the
    display under them shows, and the tab's controls sit in a row under the well (INPUTS, which has none, takes the
    well down to the lens's bottom). The tab is the same for every operator and saved with the project (a preset
    leaves it as it is); a hidden tab whose knobs a source modulates has a dot in its corner, and holding a dragged
    source's handle over a tab opens it, so its knobs can take the drop:
    - **ENVELOPE** (to begin with): the operator's envelope, and its knobs. While notes play, a dashed line across
      it shows the newest voice's envelope level, with a dot where it meets the curve, as a mod envelope's card
      does.
    - **UNISON**: a display of where the copies sit (left/right by spread, up/down by detune, each drawn as large
      and as strong as Blend leaves it; a Random mode as one note might spread them), with a row of buttons along
      its bottom, each an icon named in its tooltip: the four Unison Curves (five copies' pitches as ticks), then,
      past a rule, the three Unison Pans (five copies as dots, rising in pitch, placed across). Under it the knobs:
      Unison (the copies), Detune, Spread, Blend and Rnd Phase.
    - **WARP**: the operator's scope, its warp and its note sync. Its display is a scope: the operator's output in
      the newest voice as it plays, with its unison copies together and everything that moves it (the FM it gets,
      its envelope, the modulation on its level and pitch), before it reaches the filters, shapers or the output. It
      is caught at the operator's own phase (a synced one's at the note's), so a span starts where a cycle does and
      stands still even under heavy FM. The buttons in its corner show 1, 2 or 4 cycles across (2 to begin with):
      real cycles, so a modulator at a ratio of 0.5 or detuned copies make them differ. The trace is drawn at its
      level, between dashed lines at full scale, its level in dB in the top left corner: following its peaks as the
      header's output scope does, up at once and down slowly, so it doesn't flicker. A slow operator, whose cycles
      take longer than a fifth of a second, is drawn as it goes, up to a line, what is left of the last span faint
      after it; one whose spans come more than 15 ms apart blends each span into the next, as the output scope does.
      With no note playing, the wave's plain shape shows faintly; warped or synced, the wave as they make
      it (synced, the note's cycles, the operator's own restarting in each) shows in the accent over the plain one.
      The warp's name and amount, and NOTE SYNC while synced to the note, run along the display's bottom. Under it,
      the warps as buttons, each a sine as it warps it (named in its tooltip), the Amount knob and the NOTE switch
      (Note Sync, named in its tooltip). The audio thread captures only while the tab is shown. The summary line names the warp and says "note
      sync" while either is on. (The scope and the warp had a tab each before; a layout saved on the scope's opens
      this one.)
    - **INPUTS**: a row per node routed into the operator (the nodes that are on, in the graph's order): its name,
      which shows it in the inspector as a tab does; four buttons with the modes' glyphs, the one in force lit, each
      naming its mode on hover; the mode's name; and the route's depth as a label, dragged, typed and modulated as a
      wire's. Its feedback comes last, with its depth and "phase only". With nothing routed into it, a line says
      so. The rows sit 30 px apart, room for a modulated depth's dots under it; past the six the tab holds, they
      scroll, by the wheel over them, by dragging with the middle button held, or by the scrollbar at their right.

    A noise operator shows Slope and Spread in the Ratio readout's place, and no Pitch, Detune or Phase; its first tab is
    **SPECTRUM**, its spectrum against white, pink and brown, and it has no WARP tab (picked, it shows its
    envelope).
    Its circle in the graph is labeled with its color instead of a ratio. The envelope is a curve with draggable
    points: the peak (attack), the end of the hold just after it, the sustain level (decay and sustain) and the end
    (release). The release starts at the sustain level's point: the sustain has no length of its own, lasting as
    long as the key is held, so the display gives it none. The small dot halfway along the attack, decay and release bends that segment (drag up or down,
    double-click to straighten it, to 0 %). A hovered or dragged handle's values show at the display's top right,
    over the curve. At rest the display zooms to fit the whole envelope across its width, so the handles of
    short segments stay apart. Once a handle, or one of the envelope's time knobs, is dragged, the zoom holds still:
    while the drag goes on, and after it as long as the mouse stays over the display, so a handle stays where it
    was put and the others don't move. A handle stops where the envelope's end meets the display's end; pushed on
    past there, the display zooms out a little at a time, the further past the faster, and the segment grows with
    the handle still at the end. A knob turned so far that the envelope would run past the end zooms it out to keep
    all of it in. Once the mouse leaves the display, the zoom eases back to filling it over a few tenths of a
    second. The same goes for the mod envelopes' displays. In the graph, an operator with unison is drawn as a stack of rings.
  - Shapers: the summary (its curve, what feeds it, the filters it feeds, whether it goes out and the operators it
    modulates, as a filter's has), its curve's list with the link switch beside it, and for a Custom curve the
    symmetric one between them (as on the shaper effect's card), the Drive, Output and Mix knobs, centered under them
    (what goes straight out is its line to OUT's label),
    and its transfer curve; a Custom curve shows its points and bends there to drag, like a shaper effect's card, and its
    edit button (a pencil) opens the big curve editor over the routing panel, the shaper's page staying beside it. Underneath
    are the send knobs of the operators and filters that feed it.
  - Filters: the summary (its type and slope, then what feeds it, what it feeds and whether it goes out, and the
    operators it modulates, as a shaper's has), type buttons across the row under it, each a small picture of its
    response (named on hover), cutoff, resonance, key tracking, drive and mix (its output level is its line to OUT's
    label), and a live response curve, drawn as its Mix blends what goes in back in (a notch at 50 % dips about 6 dB,
    a low-pass leveling off 6 dB down past its cutoff), whose
    handle drags cutoff (sideways) and resonance (up/down), with the slope's list under it, as the filter effect's and
    a split's are: 6, 12, 18 or 24 dB (a band-pass's or notch's shows 6 and 18 faintly and can't pick them, and shows
    what plays; for a comb it is faint and doesn't open). With its cutoff or resonance modulated, faint curves
    show how far the modulation takes it either way, and while notes play the curve follows where the newest
    voice is (the handle stays at the set values, the cutoff a C4 has). Key tracking moves all of it with that
    voice's note, an octave per octave at 100 %. The filter effect's display does the same, its Mix drawn in too, with a handle of its own for cutoff and resonance, its key tracking
    staying where the last note left it, as the effect's does. It sits on the filter effect's grid. Behind the curve,
    like the filter effect's, is what the filter puts out, all voices together, measured before its output level
    (the Param EQ's analyzer: a 2048-point FFT in 96 bands, 0 dB at the top), while the editor is open; a filter
    that is off shows none. Underneath are the send knobs of everything that feeds the filter.
- **Modulation**: the panel's title row has the **MODULATION** toggle, which collapses the panel to that row and
  opens it again (the editor gets shorter; saved with the project, but not part of presets), then **+ SOURCE**,
  shaped like + EFFECT (a drawn plus, the word and a chevron), which opens the list of source types under it to add
  one (which opens a collapsed panel), and beside it how many of the 16 places are taken, as in 4/16 (with every
  place taken it is dimmed, and a click says so). Random and Curve cards are laid out as an LFO's (see below), but
  have **Unipolar** | **Bipolar** (as icons) where an LFO's shape list is, one of them lit, which sets which way their
  depths swing, as the polarity in its title's menu does, and no Unipolar switch; a Random card has no Phase knob. A MIDI card has its input's list (CC,
  Pressure or Slide, where an LFO's shape is; its tooltip says what the input follows), and right of it its CC knob
  (while its input is a controller) and Slew; under them, both cards'
  display is a bar of what the newest voice gets now (from the middle while the source is bipolar), with its value.
  Below, the sources' cards show four at a time, and after the last one, while there is room for more, a dashed
  card with a plus, **ADD SOURCE**, as tall as the cards, opens the list of source types at the mouse, moved back
  inside the editor where it would reach past an edge (Envelope, LFO, Curve,
  Random and MIDI, each with a small picture of what it puts out); a pick adds one, and a click elsewhere closes
  the list. With more than fit, the scrollbar under them (thinner while the mouse is away from it, like every
  scrollbar) or the mouse wheel over the panel scrolls them smoothly, and a drag with the middle button held drags
  them along. Sources are numbered by type in the order they show ("LFO 2", "Curve 1", "Env 1", "Random 1",
  "MIDI 1").
  - Drag a card by its title (anywhere in the row but its handle and buttons) to move it along; held at either end of the row, it scrolls the row along that way. A faint image of it follows the mouse anywhere, over the other panels too, and only its outline shows where it would land; the other cards make room only once the mouse is clear of the card's own place, where the mouse is.
    With Ctrl held as it starts to move, it carries a copy instead, as Duplicate makes one (no targets), which
    stays right after the original while the mouse is over it and lands where it is let go of: one undo step.
    The order is saved with the patch. Right-click the title (or the handle) to duplicate the source (with its
    settings and shape, but none of its targets: the copy modulates nothing until it is given some), clear its targets, make it **Unipolar** or **Bipolar** (the one in force checked) or
    remove it; the × removes it too, together with its modulation and whatever modulates its knobs. A bipolar
    envelope's display has a dashed line at
    its middle level and ± in its corner. While notes play, an envelope's display has a dashed line across at the
    newest voice's level, and a dot on the curve as far along the stage the envelope is in as it has come: along
    the attack, decay or release, across the hold, and at the sustain level's point through the sustain. While the
    envelope follows its knobs, the dot sits where the line meets the curve. Where it doesn't (a note let go
    during the decay releases from above the sustain; modulation moves the sustain), the dot keeps to the curve
    and the line shows the level.
  - A card's title row is laid out as an effect card's: the handle and the name in capitals ("LFO 2", "ENV 1") at
    the left, then the fold chevron, the power switch and the × at the right.
  - The power switch turns a source on and off; one that is off has its card dimmed, its name grey (as an effect's is), and its handle and its rings
    on knobs faint (the handle can still be dragged onto targets). Like a macro's, a source's handle is faint too
    while it modulates nothing (and scales no modulation as its aux).
  - The chevron left of the power switch folds the card to a narrow strip, as on the effects: the handle, a grip
    to drag it by, the switch and ×, then the name, which unfolds it: its letters down the strip and its number
    across (a Random source's shortened to RND, a Curve source's to CRV).
  - Each source has a colored handle in its card's title row. Drag it onto any knob, wire label
    or matrix cell; a filter's cutoff is the classic target. Held near the left or right end of the modulation
    panel or the effects chain, it scrolls those cards that way, faster nearer the edge, so a knob on a card out of
    view can be reached. Hovering the handle draws a dashed line to each of its targets on screen, ending at the
    small dot it has there (see below), with a ring round the dot; while the source has no targets, it shows a
    "Drag to assign" tooltip instead, as the macros', the note source's, the wheels' and velocity's handles do.
    A source that is another modulation's aux (see below) also gets a dotted line, in its color and bowed the
    other way, to that modulation's depth handle on screen, which is ringed with dots.
  - A card doesn't list its modulations, and ends with a button under its display: it counts them, with **MODS ›**
    at its right, and shows them in the MODS view, scrolled to the source's first row. While the source modulates
    nothing, it reads **No modulations** and adds an empty modulation from the source there (see the MODS view),
    with its target's menu open.
    A Random
    card has **Smooth** and **Chaos** knobs beside its display, which shows four made-up cycles of what they do;
    while notes play it shows the source's own last four cycles instead, as one line scrolling left with the live dot
    at its right end (at the end of a cycle, it waits there until the next one comes in). Standing still at 0 Hz,
    it shows four made-up notes' values, and while notes play a line across at the value the newest voice holds,
    the live dot at its right end. A Free or Anchor LFO
    runs on one clock whether notes play or not, so its card shows its dot moving (and a Random one its cycles
    scrolling) with no note playing too; a Retrigger or One-Shot one, which notes start, shows none until one
    plays. Without a note it modulates nothing, so only the card moves, and only the card repaints. While the editor
    is open with one switched on, the CLAP plugin keeps processing with nothing sounding, where a host would let it
    sleep and the clock would stand still. An LFO picks its shape from a list, like a
    shaper's curve, its button and each entry showing the shape's icon before its name (the wheel over it steps
    along them). Under the shape list, as
    wide as it, are its clock modes side by side, as Poly | Mono are, the one it runs in lit, each drawn as an icon
    (named on hover): a rise that starts again at each dot for **Retrigger**, an infinity sign for **Free**, an
    anchor for **Anchor** and a single cycle that ends in a dot for **One-Shot** (a Random card's two, Retrigger and
    Free, each twice as wide). A click picks one (a click on the
    one lit leaves it), and the wheel over them steps to the next one. Right of those two rows, centered
    on them, are the Rate and Phase knobs (a Random card's Rate alone),
    over a Random card's Smooth and Chaos, with the sync switch between the shape list and Rate, level with the list: a small square button
    with the metronome the effects' SYNC has, which turns the Rate knob into a note-value knob (as it starts, on).
    Under it, level with the clock modes, is the **Unipolar** switch, a hump on a baseline as a MODS row's
    polarity switch has.
    The Phase knob is faint while it does nothing (on a free LFO that isn't anchored), and otherwise
    a thin line on the display marks where notes start the LFO (or where the song starts an anchored one). A
    unipolar LFO's display has its zero line at the bottom. A Curve source's
    display, or the expand button in its title row, widens its card, to edit the shape there (see the shape editor);
    a new one starts widened.
  - A new modulation starts at +30 %, upward, whatever the target's setting. The target then shows a colored ring
    for the modulation range, plus a small dot of the same color under the
    knob (on a matrix cell, centered along its foot, the ranges along its top, right of a mode's glyph; on a wire or OUT label in the
    graph and on the INPUTS tab's labels, centered just under it, the ranges running along its outline's top, so
    its depth stays readable). Where a range runs past its target's end, cut off there, a short line across the
    ring or bar marks that end (in every theme; the Ratio readout's track has its arrow instead). The dots stay
    inside their knob's column, label or cell at each handle size; past
    what fits, the rest go into a "+n" pill. Drag that dot to set the depth; double-click it
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
    "…"). It shows six rows at most; past that, it is 8 px wider for a scrollbar at its right, and the rows scroll by
    the wheel over them (off the depth bars, which the wheel turns), by dragging with the middle button held, or by
    the bar. A click anywhere else closes it, but not
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
    note source, the mod wheel, velocity or the pitch bend); so do **Bypass** / **Resume** in the right-click
    menus of the row, the
    knob's dot and the modulated knob. A bypassed modulation's dots are hollow, its row and ring dimmed. Bypasses
    are saved with the patch (as params of the editor's own, like the drawn shapes).
  - A modulation's ring on a knob, and the bar on a label, cell or depth bar, show the range it swings through: both
    ways of the set value while its source is bipolar, or one way while it is unipolar.
  - While notes play, a white dot on each modulated knob shows where the newest voice currently is; with no note
    playing, it shows where the macros and the mod wheel take it, as they apply with or without notes, and on an
    effect's knob and Volume, while the effects ring out after the last note, where its voice's modulation takes it.
  - Each depth bar (in the MODS view and a pill's list) has a white tick where its modulation takes its target now,
    on the bar's own scale, so a unipolar one at the top of its swing reaches the depth's handle: the newest voice's
    while notes play, and a macro's or the mod wheel's from its knob, notes or not. A modulation another source
    must run per voice shows none with no note playing (but on an effect's param or Volume while the effects ring
    out after the last note), nor does a bypassed one or one whose source is off.
  - A modulated route shows it too: its wire (or its line to OUT), in its own color as any other,
    swells and thins with the modulation while notes play (with the macros' alone between notes), following it as the
    graphs' glows do, rising with a time constant of 70 ms and falling back with one of 100 ms, so a fast modulation
    shows as a steady swell rather than a flicker; its matrix cell has a small bar along its bottom with each
    modulation's range, a tick where the amount is set and a white dot where it is now, and its label each
    modulation's range just above it, clear of its solid fill.
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
  panel that scrolls (the effects chain, the source cards, the MODS view's rows, an operator's INPUTS rows, the
  preset browser's list, a pill's open list) to drag what it shows along with the mouse, whatever is under it; past
  either end the row stops, and dragged back it follows at once. A middle press on another open list or menu does
  nothing, and one that starts a drag closes it;
  while the left button drags something, the middle one does nothing. Two fingers on a touchscreen pan the same
  panels as the middle button, along with them (in the browser demo, on a phone too): what the first finger pressed
  ends as they land, nothing dropped, and over a dialog they pan nothing. A precision touchpad's two-finger
  scrolling comes as the wheel, up and down as above and sideways as a tilted wheel does, which scrolls what scrolls
  sideways under the mouse (the effects' row and graph, the source cards) and turns nothing. In the browser demo on
  a phone (or any touchscreen), one finger works the editor too: where nothing scrolls it is the mouse at once;
  over a panel that scrolls it waits until it moves or lifts, so that dragged on something that drags (a knob, a
  depth bar, a card's title or icon, a handle, a node) it drags it, dragged anywhere else it pans the panel, and
  lifted without moving it is a click where it landed (two quick taps a double-click, so a double tap sets a knob
  back). A long press opens the menu a right-click would. Every dialog (the
  confirm dialogs and the settings) takes the keyboard while it is open: Enter presses its button lit in the accent
  (Load, Init, Overwrite, Clear FX, Keep), Escape cancels it as a click around it does, and both close the settings;
  the dialog asking where a middle band's effects go lights none, and Enter leaves it open. A double-click whose first click closed the
  preset browser, a dialog or a list doesn't act on what was under it. Double-click the number a knob or a target's depth shows (under
  the knob, or beside it in the header), or pick **Edit value…** (**Edit depth…** on a modulation) from the
  right-click menu, or Ctrl+Click it, to type a value, in the units it shows or as a plain number (`300` on a
  time is 300 ms, `1.2k` on a cutoff 1.2 kHz; `Off` or `0` on a time that can be Off, while `1 ms` is the shortest
  time; on an envelope's attack, decay or release `0` is 0 ms and `0.5` half a millisecond, and `0` on an LFO's, a Random source's, a phaser's, flanger's or chorus's Rate stands it still, while `0.02 Hz` is
  its slowest sweep); Enter or a click elsewhere sets it, Escape drops it. The editor
  takes the keyboard while you type and hands it back afterwards; the keys are taken straight off the window's
  message queue, so hosts whose message loops pass keys on to themselves (Bitwig) don't get them first. A wire
  label, OUT label or matrix cell types a value on Ctrl+Click, since its double-click removes or toggles the route.
- **Menus and lists**: every list and menu (the effects', the source types', the waves', a list button's, a Param EQ
  band's shapes, the MODS view's cascading menus) marks the row under the mouse, and the one picked, in the same
  neutral tones; only a row's icon takes a color (the effect's, the band's, the source's).
- **Button rows**: buttons that pick one of a few (Poly | Mono, an operator's waves, a voice filter's and the filter
  effect's types, an LFO's clock modes, the partial counts, Points | Steps, the phaser's and flanger's + | −, the
  chorus's voices) and
  switches that sit side by side (the
  delay's Sync, Link and Ping-Pong, the shaper's Symmetric and Link, the 3-band dynamics' RMS and LINK) are drawn
  merged into one pill: one outline, rounded at its ends, with a thin line between its parts, each part lit on its
  own. A switch with no neighbor (Legato, Always, the power switches, the phaser's and flanger's Sync and STEREO)
  stays a button of its own.
- **Keyboard strip**: along its top the **KEYBOARD** toggle, the macros close together, the note source and, at the
  right end, velocity; under them the
  wheels and six octaves of virtual keys, from C1 to C7 (the small − and + buttons under their right end, the
  range between them, shift them by an octave). Each wheel's handle sits over it, under the strip's title. The toggle
  (a chevron and the title, like the modulation panel's, as far from the strip's top) collapses the strip to its top
  row, taking the wheels, the keys and the octave shift away; the wheels' handles stay, their names (BEND, MOD) then
  under them, so the wheels can still be assigned. Whether it is
  collapsed is kept with the editor, like the modulation panel's, and loading a
  preset leaves it as it is. Drag across the keys to glide between notes. Keys light up while they are held, whether clicked here or
  played by the host. The **Bend** wheel is the Pitch Bend param: it bends the pitch by the Bend Range and springs
  back to center on release, in one edit the host follows. It shows where the bend stands: the param, with what the
  host adds on top (its MIDI pitch bend, and its modulation of the param, the newest note's own while notes play),
  filled in the pitch bend source's color, and its handle drags the pitch bend source onto what it should move. The
  **Mod** wheel is the Mod Wheel param, and its handle drags onto what it should move (not the wheel itself). Both
  handles work like a macro's (faint, like theirs, until they move something). The mod wheel's and either handle's
  right-click menus make that wheel unipolar or bipolar; a bipolar mod wheel fills from its middle, where it moves
  nothing.
- **Macros** (in the keyboard strip, above the keys): eight knobs, each in a color of its own, with a handle
  beside it to drag onto anything a source can modulate, just like an LFO's, and its name under it: double-click
  the name to change it (Enter
  keeps it, Escape doesn't, an empty name brings back "Macro 1" and so on; an undo step, saved with the patch).
  Hovering a handle draws lines to what it moves; the dots and rings on its targets work like a source's. A macro's
  knob takes modulation like any other knob (a source's handle dropped on it, its depth handles under it), but not
  its own macro's handle nor one that would loop, which says why as it is let go of; its dot then shows where the
  newest voice's modulation takes it, or with no note playing where the other macros and the mod wheel take it. Until a
  macro modulates something or scales a modulation as its aux, its handle, its knob's arc and its name are faint,
  and hovering the handle shows a "Drag to assign" tooltip (so do the note source's, the wheels' and
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
  BA bass, DR drum, FX effect, KY keys (bells and organs too), LD lead (brass too), PD pad (moving textures too),
  PL pluck and SQ sequence (rhythmic, gated). They are KY Electric Piano, BA FM Bass, KY Glass Bell, PD Warm Pad, KY Drawbar
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
  to buzzing reed, a mod envelope's bite on each note and a vibrato that a second envelope, as its aux, fades in);
  PD Six-Op Cascade (all six operators in one chain, F down to A, four slow LFOs breathing its links at rates that
  never line up, and an Index macro deepening them all); BA Neon Pluck (a mono saw plucked by FM through a
  resonant low-pass over a sine sub, a two-bar LFO turning its FM and grit, its effects as a graph: a split at
  180 Hz whose high band is widened and echoed while the low band stays tight, then a 3-band dynamics set as OTT;
  its Timbre, Filter, Grit and Space macros). Then two more for each category, one in the style of 1980s FM synths
  (sine operators only, indexes falling fast and scaled by velocity) and one modern and heavy: BA Eighties Slap (a
  1:1 pair with feedback for the body, a 9:1 click for the thumb and a sub sine) and BA Grind Bass (a fed-back pair
  growled by a sub-octave linear FM through an asymmetric shaper and a driven low-pass, over a clean sub, its effects
  as a graph that keeps the lows below 140 Hz tight while the highs are clipped and widened; its Growl, Drive and
  Width macros); DR Log Drum (a tuned log drum, a 3:1 knock dying in a blink as its pitch drops into the note) and DR
  Hard Kick (a fixed-pitch sine swept down from about 600 Hz into an asymmetric shaper whose tail distorts, under a
  noise click; its Pitch, Distortion, Length and Click macros); FX Jet Takeoff (a modulator at full feedback, driven
  by an inharmonic one, roaring through its carrier, noise made by FM alone, a slow pitch envelope lifting it two
  octaves under a flanger) and FX Crush Downer (an FM growl folded by a sine-fold shaper and falling three octaves
  onto the note under band-passed noise falling with it); KY Funk Clav (a 1:1 pair with feedback for the nasal buzz
  beside a 2:5 pair for the pluck) and KY Driven Tines (an FM tine piano through an asymmetric shaper, a cabinet's
  band and a stereo phaser, velocity driving the amp too); LD Classic Synlead (a fed-back 1:1 pair's saw beside a
  2:1 pair's square, detuned, with a bite on each note and a delayed vibrato the mod wheel deepens) and LD Screamer (a
  square swept by a 2:1 modulator and unison saws, sine-folded and torn through a driven low-pass, the mod wheel
  screaming the fold and feedback up); PD Vintage Strings (three fed-back pairs bowing in behind their carriers, an
  octave on top, a slow drift on the indexes, chorused into a hall) and PD Distorted Swell (wide unison saws growled
  by a sub-octave modulator through a driven low-pass into a sine-fold shaper, a slow LFO and a long envelope
  opening it as it is held); PL Koto Pluck (a fed-back 1:1 pair and a 7:1 snap on one carrier, a 2:3 pair ringing an
  octave up) and PL Razor Pluck (a fed-back pair driven by a 2:1 modulator through a snapping resonant low-pass over
  a sub and a noise click, hard-clipped, squashed OTT-style and echoed); and SQ Mallet Steps (a marimba, a 1:1
  carrier knocked by a 4:1 modulator, struck every sixteenth by a drawn LFO with its own accent each step while a
  second steps the pitch through octaves, fifths and sevenths; its Melody macro flattens the tune) and SQ Hammer Gate
  (a fed-back pair growled by a sub-octave linear FM through a driven, resonant low-pass into a hard clipper whose
  output a drawn LFO gates in accented sixteenths, a second jumping octaves in the riff).
  Then three that play through the Amp: PL Crunch Rhythm (a string-like FM pluck, two carriers a few cents apart,
  a feedback modulator for the saw's edge and a short bright one for the pick, velocity digging in, through the
  Crunch amp into a closed 4x12 and a small room: a rhythm guitar for chords); LD Overdrive Lead (a mono, gliding
  lead through the Lead amp into an open 2x12, a vibrato that an envelope fades in as a note holds, the mod wheel
  turning up the amp's Gain, then ping-pong echoes and a plate); and BA Driven Bass (a round FM bass with a sub an
  octave down, through the Bass amp's grit into a 4x10, its lows staying clean). A folder made
  before an example came gets it the next time the library is used; `%APPDATA%\FMVibe\settings.txt` counts the examples
  given, so deleted ones stay gone. It also notes the version of FMVibe that wrote them out: a newer one writes the
  factory presets still in the `Factory` subfolder again as it has them, so they take its tuning and its routing
  graphs' layouts (a change saved over one of them is lost then; save it under another name to keep it), and leaves
  those taken out of it out. Their routing graphs are laid out with no node on another, and no wire, line to OUT or
  label running over a node it doesn't join. The plugin carries the factory presets built in and writes them out as they are:
  they hold only the params that differ from Init, so their files are small, and load like any other.
  - The **preset bar** in the middle of the title bar shows the preset loaded or saved last, or **- Init -** (as
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
    **Save As** (a disk with a plus) read and write a `.fmv` file through the standard file dialog, which always
    starts in the preset folder, wherever the host's last file dialog was left; Save As suggests the current
    preset's name. The file holds the same data as the host's
    saved state, apart from the settings.
  - **Save** (the disk) writes the patch over the file of the preset it is, after asking ("Overwrite …?"), and
    tells in a message that it saved or couldn't. It is on only for a preset loaded (or saved) from a file that has
    changed since; otherwise, and always for Init, it is greyed out and does nothing. Save As is greyed out too on
    Init until the patch changes. In the browser demo, which has no files, both say so.
  - Init and loading ask first only while the patch has changed since it was loaded, saved or reset, or when the
    host brought a patch that isn't a preset, since the patch they replace can't be undone back (undoing every edit
    since a preset loaded leaves it unchanged again). The project remembers which preset it was and whether it changed.
- **Settings** (the cog after Save As): **Oversampling**, 1x, 2x, 4x or 8x. It is saved with the project but not
  with presets, and changing it restarts the plugin (the host deactivates and reactivates it).
  **MPE**, Off or On, and its **Bend** range for the member channels, 12, 24, 48 (to begin with) or 96 semitones,
  faint while MPE is off (see MPE, above). Like Oversampling, they are saved with the project but not with presets,
  and aren't undo steps.
  **Theme**: Spectrum (deep indigo, the default), Porcelain (Spectrum in light: white panels lifted off a cool gray
  by soft shadows, their cards with a slight one too), Midnight or Light (crisp: white cards and knob caps on a cool
  gray, with deep colors), each button shown in its own colors. The browser demo's page around the editor is in
  Spectrum's colors. Spectrum and Porcelain share a flat look: each
  effect and source card tinted with its own color and outlined only while the mouse is over it (or it is dragged),
  a split's container and its card in the effects graph likewise, on the same shadow as the cards beside them, the
  band it shows faintly in its color behind its cards; a knob's arc, and faintly its track, in the color of what it
  belongs to (its card's; in the inspector a filter's and a shaper's, and the routes into them on its INPUTS tab, in
  that node's color, as its switches are, an operator's in the accent), modulated or not: a modulation ring whose
  source's color is like the arc's is drawn lighter (in Spectrum) or darker (in Porcelain), keeping its hue, so it
  stands apart from the arc, while the others keep their sources' colors; and its pointer a dot; an LFO's shape filled on
  its card; a lit switch filled solid with its color, rows of them set in a well (the LFO's clock and polarity icons
  a little smaller there), and so the split's 2 | 3, the scope's cycle counts, the Param EQ's band chips (the picked
  one tinted with its band's color), the octave buttons and the dialogs' buttons; list buttons, a source card's MODS
  button, the lists and menus they open, tooltips, the name tags and the messages (on a soft shadow, white in
  Porcelain and a step lighter than the rows in Spectrum, their rows shaded under the mouse), the title bar's
  buttons, the preset's name, the output scope, the Ratio readout (outlined under the mouse), the note source's and
  velocity's curves and the wheels without outlines, and search fields outlined only while typed in or holding
  words; the dialogs lifted by a shadow in Porcelain, as its panels are; + EFFECT, + SOURCE and the MODS view's + Aux
  and + Add modulation as filled pills, an aux that is set a chip tinted with its source's color, and the cards that
  add an effect, a source or a band's effect filled faintly, a disc behind their plus; each operator, its tab, its
  names in the matrix (whose legend shows the operators' colors side by side) and the routes it sends to another
  operator in a color of its own (a route into or out of a filter or a shaper takes that node's color, as in every
  theme); the routing nodes on a soft shadow (a silent one outlined in a faint tint of its color, and an operator's
  unison copies tinted discs peeking out on both sides, under the wires so an arrowhead coming in at its side shows);
  the effects graph's nodes, a split and a join too, filled
  with a tint of their color, without an outline; and meters and a dimmed control taking the tint of the card they are on. Settings
  that name a theme no longer offered show the nearest one: Graphite and Machined as Midnight, Paper as Light.
  **Hue**, a slider under the themes along every hue, turns the theme's main color, its accent (and in Spectrum and
  Porcelain operator A's, which is the accent there), round the color wheel, as saturated and as light as the theme
  has it: Default to begin with, the theme's own color, and its hue in degrees once one is picked. It is kept as the
  theme changes, each theme taking it in its own saturation and lightness.
  **Glow**, a slider from Off to 100 % (50 % to begin with), makes the curves in the graphs (envelopes, LFOs, filter
  and effect responses) glow, drawing them a little thinner: the higher, the stronger and wider the halo. It sets the
  halos that sounding operators and the effects graph's nodes and wires feather out with too: as they are drawn at 75
  %, stronger and wider above it, fainter below. In a light theme (Light, Porcelain) a grey wire of the effects graph
  glows in the color of the effect it comes out of (or the accent), since a grey glow would read as a shadow there. At
  Off the effects graph shows no level (and the editor doesn't repaint for it), and an operator's halo is a plain
  ring.
  The editor shows it while it is dragged and keeps it when it is let go, as it does Hue and Lines; a double-click on
  any of the three sliders puts it back to where it starts (Hue to Default). **Lines**, a slider like it from Off to
  100 % (75 % to begin with), sets how strongly the dashed lines from a source's handle to what it modulates (and the
  one a handle being dragged trails) are drawn: Off draws none, 100 % draws them opaque. **Handles**, Small (to begin
  with), Medium or Large, draws the depth handles and their **+n** pills bigger, each step about half a pixel more in
  radius and a pixel more across, in the same places (so on a label or cell, Large ones overlap a little). All five
  are yours rather than the
  project's: they are kept in `%APPDATA%\FMVibe\settings.txt`, every FMVibe editor uses them, and other open editors
  switch along. An editor writes only what it changed into that file, leaving what another one changed meanwhile. The dialog's bottom line shows the version number and that FMVibe is released under the MIT
  license. The editor's size is kept there too (`zoom`), written when an editor you resized closes: a new instance
  opens at it, while a project that saved a size keeps its own.

### Remote-control pages (Bitwig, CLAP)

| Section | Pages |
| --- | --- |
| Mix | Macros (the eight knobs, the page shown first), Global (Volume, FM Amount, Pitch, Pan), Play (Mono, Legato, Glide, Glide Always, Bend Range, Voices), Outputs (A–F, then S1 and S2 To Out) |
| Operators | Op A … Op F: Wave, Ratio, Pitch (Fixed Freq and Fixed Mult while the operator is fixed), Detune, Attack, Decay, Sustain, Release |
| Filters | Filter 1 and 2: Type, Cutoff, Resonance, Key Track, Drive, Mix, On, Slope; Filter links: F1 Feedback, F1 → F2, F2 → F1, F2 Feedback |
| Shapers | Shaper 1 and 2: On, Curve, Drive, Output, Mix, Link, then what it feeds into F1 and F2 |
| Matrix | "A modulates" … "F modulates": what that operator modulates; "Into F1", "Into F2": each operator's send into that filter; "F1 modulates", "F2 modulates": the operators each filter modulates; "Into S1", "S1 modulates" and the same for S2: each operator's and filter's send into that shaper, and the operators it modulates; "Shapers into shapers": S1 Feedback, S1 → S2, S2 → S1, S2 Feedback |
| Modulation | One page per source, in the editor's order: its first 8 slot depths |

## CLAP and VST3

FMVibe comes as a CLAP plugin (`FMVibe.clap`) and a VST3 plugin (the `FMVibe.vst3` folder), built from the same code:
the same engine, editor, parameters (with the same ids), presets and settings. The VST3 plugin is written straight
against the VST3 interfaces, with no wrapper in between, as one object that is both its processor and its edit
controller. Its state is the same bytes as the CLAP plugin's, so a `.fmv` preset loads in either; hosts see them
as two different plugins, though, so a project made with one doesn't switch to the other by itself.

Hosts are given 1,298 of FMVibe's 19,641 parameters: the globals, the operators, the matrix and the routes' modes,
the filters, the shapers, the macros' knobs, the note source's, the mod wheel's, velocity's and the pitch bend's own,
and every modulation's depth and Amount (16 slots for each of the 28 sources, used or not). The rest are set in the editor only:
the effects, the LFOs', envelopes' and other sources' own knobs, what each modulation targets and its aux, curve and
bypass, the Curve sources' drawn shapes, the shaper curves and the note source's curve, the Custom waves' partials and their
counts, the effects graph (its splits, joins and wires), the chain's retired order, the feedback's route modes and the
retired fields. They are
saved with the project and in presets like the others, the editor's sources modulate them and its history undoes
them, but hosts can't automate them and don't record their edits: an edit of one marks the project changed
instead.

What the VST3 plugin does its own way:

- **MIDI**: VST3 hands plugins notes, but no controllers. FMVibe asks hosts to map every controller, channel pressure
  and pitch bend, on each of the 16 MIDI channels, to hidden parameters of its own, and takes them as MIDI from
  there: pitch bend (by Bend Range), the MIDI sources' controllers and pressure, and MPE over MIDI work as in the
  CLAP plugin. Polyphonic pressure comes as its own event, a note's pressure.
- **Note expressions**: Volume, Pan, Tuning (ten octaves either way) and Brightness (a note's slide), as VST3 names
  them, and Pressure, one of FMVibe's own. For touch surfaces and MPE controllers, FMVibe tells hosts to send a
  finger's sideways movement as Tuning, its movement along the key as Brightness and how hard it presses as
  Pressure. A note-on's tuning (in cents) tunes the note from its start.
- **Parameters** are grouped in units, as their modules are (Operators, Op A, Filters, ...).
- **Restarting**: raising the voice limit past the voices allocated (a project loaded may too), or changing the
  oversampling, asks the host to deactivate and reactivate the plugin (through a latency change, which has hosts do that).
- **Not in VST3**: CLAP's per-voice and global host modulation, Bitwig's remote-control pages, and the note-end
  reports that per-voice modulation needs.
