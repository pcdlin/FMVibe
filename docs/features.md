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
  - **Drawn envelopes** (EnvMode, ADSR to begin with): an operator's envelope can be a drawn curve instead of its
    AHDSR, a shape of up to 64 points (`opEnvShapeIndex()`, ids 81000 + 200·op) whose level goes from 0 at its
    bottom to full at its top, played over its **Length** (1 ms to 10 s, about a second to begin with), or synced to
    a note value at the tempo. **One-Shot** plays it once from each note and holds its end while the key is down;
    **Sustain** (to begin with) loops its loop region, the whole curve to begin with, while the key is down. Let go,
    it goes on at once from the level it is at to the curve's end, as a Curve source's Sustain does (an offset fading
    by the next point, or a 5 ms glide where the loop ends on a drop or at the curve's end), and a curve ending above
    0 fades out over 5 ms there, so the voice can end. It is stepped per sample (`DrawnEnvelope` in
    `src/drawnenvelope.h`, which `Envelope` follows in a stage of its own, so the voices' loop calls it as before):
    each segment's bend kept as an exponential multiplied each sample, not worked out again. **Slew** (Off to begin
    with, up to 1 s) lags what it puts out behind its shape, a one-pole lag a sample at a time, so a jump drawn in it
    or its loop wrapping from one level to another doesn't click; slewed, once its end has faded out, what it still
    puts out runs down to nothing in a straight line, as steeply as the lag was taking it, so within the Slew's time,
    and its voice ends then. Until it first reaches its curve's highest point it counts, for voice stealing, at its
    top, as an attack does. Its fields and shape are the editor's alone (hidden from hosts); Length and Slew can be
    modulated, at the control rate. An operator on its AHDSR costs what it did;
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
  - **Phase**: where the waveform stands, 0 to 100 % of a cycle (0 % by default; a sine at 25 % starts at its peak).
    A note starts there, with the host's and the sources' modulation on it then. Turning the knob, automating it or
    modulating it moves the notes already sounding along with it at once, without a click: a jump goes the short way
    round the cycle, and modulation carries on past 0 % and 100 % rather than stopping there, so an LFO on Phase is
    phase modulation at any setting of the knob. A noise operator has none. Operators don't follow velocity
    themselves: velocity reaches them as a source (see Velocity, under the modulation sources), aimed at Out or
    wherever else;
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
    keeps what aliases 65 dB down at the default 2x oversampling. Its scope (on the Warp tab) follows the note's
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
    - **Sync**: the wave played up to 16 times through each cycle (1 + 15 × the amount's size, either way), cut off
      where the cycle ends, as Serum's Sync warp: an oscillator hard-synced to the operator's own pitch, its formant
      sweeping up as the amount rises while the pitch stays. At whole numbers of times (2/15 is 3 times) it plays
      that harmonic alone. Its step at the cycle's end is smoothed as Note Sync's restart is, so a sine at C6 keeps
      what aliases 70 dB down at 2x at 50 % (a saw at -80 %, cut through its own edge, 90).

    At 0 every warp is the plain wave, bit for bit; the most it squeezes a stretch is 8 times (Sync's 16), at
    ±100 %. A warped operator reads a table with as many fewer harmonics as its warp squeezes the cycle at its
    steepest, and the bends a warp puts in the wave's slope are smoothed as a sync's restart is, so even Steps at C6
    keeps what aliases about 60 dB down at 2x. A state from before version 15, when Sync reached 8 times, loads its
    amount and the depths on it 7/15 as large, playing as it did. Warp Amount can be modulated (pulse-width modulation is an LFO on Squeeze); it
    glides to each control block's value, moving at most its whole range in 8 ms, and a warp changed to another
    first goes back to the plain wave and then out to the new one, so neither clicks. Warp and Note Sync work
    together: the restarted wave is read through the warp. An operator with neither runs the plain loop it ran before,
    the synced and warped ones a loop of their own;
  - **Rnd Phase** (phase randomization): at 0 % (the default) every note starts the waveform, and each unison copy, at
    Phase, so notes sound alike from the first sample; turned up, each copy starts up to that much of a cycle
    later, at random. Unison copies at 0 % start in step and drift apart as they detune. Each instance draws its
    randomness of its own (here, a noise operator's noise, Random sources and the unison modes'), so instances layered
    don't play the same noise, and a noise operator comes out a little different from one bounce to the next.

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
  that only modulates can leave its output level at 0. Modulation moves a filter smoothly: its cutoff, key
  tracking, resonance, drive, vowel and morph ramp over each 32-sample control block, the filter's settings worked
  out again every 8 samples while they move (every sample, from where the ramp has them, while an operator moves one
  of them at audio rate), and what it scales its outputs by (a morph's shares, a formant's levels, the resonance's
  share of a high-pass, band-pass or notch) gliding sample by sample, so a fast envelope or LFO on the cutoff sweeps
  it rather than stepping it every block (which buzzed faintly at the steps' rate, 1.5 kHz at 48 kHz:
  some 72 dB under an LFO's sweep, 53 dB for a morph's place, now 96 dB and more). Its type, slope and morph mode switch
  at once. The editor shows the types as seven families (Low-pass, High-pass, Band-pass, Notch, Comb, Formant and
  Morph), the first three each a family of models: the low-passes SVF (the state-variable filter's), Ladder, Diode and
  Sallen-Key, the high-passes SVF, Ladder and Sallen-Key, and the band-passes SVF and Ladder (see the filter's page,
  under Editor). That is the editor's alone: the Type param stores the type, so hosts, presets and states see the
  types below. Types:
  - Low-pass, High-pass, Band-pass and Notch: a state-variable filter that stays smooth under fast modulation.
    **Slope** picks 6, 12 (the default), 18 or 24 dB per octave. At 6 dB the filter is a one-pole, taken from the
    state-variable filter so resonance still adds a peak at the cutoff (as high as at 12 dB) while the slope past it
    stays 6 dB per octave; 18 and 24 add a second, plain stage after the resonant one (a one-pole, or a
    Butterworth), so the resonance stays a single peak. A band-pass or notch comes in 12 and 24 dB only: set to 6 or
    18, it plays the slope above;
  - Comb: a tuned feedback delay, so the cutoff sets the pitch and resonance the feedback. With its cutoff at 0
    and the note source on it at 100 %, its curve drawn along the cutoff knobs' (and Key Track left at 0), it
    rings at the pitch of the note;
  - Ladder: a four-pole transistor ladder, a low-pass whose resonance runs in a loop around its four one-pole
    stages, solved without a sample's delay so its cutoff and peak land where the analog circuit's do. **Slope**
    taps one to four of its stages (6 to 24 dB), the resonance coming from around all four. Each stage saturates
    what it takes in, on a curve close to tanh, as a transistor ladder's pairs do: an operator at its usual level
    comes out warmed (a sine's 3rd harmonic about 30 dB under it, where the low-pass leaves it clean), and Drive's
    saturator feeds it at full level rather than brought back to the level the sound went in at, so its stages
    saturate on top of it (its output is brought back instead). The saturator holds what it feeds in under twice the level it holds the other types' (the
    same curve, larger), so a loud, driven sound reaches the stages' own saturation, whose first stage takes the input
    less the resonance's feedback: the louder the sound goes in, the less room is left for the feedback, and the
    resonance comes down, as in a driven transistor ladder. A full-level saw through 85 % resonance comes out of full
    Drive with its peak some 20 dB further under its fundamental than undriven, where it came down 10 dB with the
    drive held under the other types' level, as it was until 0.17; a quiet sound plays within a dB of how it did.
    Feeding the drive in with no ceiling at all would pull the resonance down hardly further and alias some 7 dB more;
    with this one a driven ladder aliases about 2 dB more than it did. A project saved before plays its driven ladders
    and diodes this way too: of the factory presets, Acid Rubber's and Cutoff Growl's, the two with a driven ladder,
    come out about 0.1 and 1.1 LU louder. As its resonance rises its level below the cutoff falls (a quarter of that
    loss put back), thinning the note. From about 93 % of Resonance it self-oscillates, a tone of its own at its
    cutoff held by its stages' saturation, louder toward the top; once a released note's operators have faded, its
    resonance is held at 90 %, so its ringing dies away and the voice ends;
  - Ladder HP: the ladder's loop around four one-pole high-pass stages, as in Moog's 904B, the low-pass ladder the
    other way round: **Slope** taps one to four of its stages, it saturates in each as the ladder does, takes Drive the
    same way, loses its top as its resonance rises as the ladder loses its bass (a quarter of that put back), and
    self-oscillates near its cutoff from about 93 %, held at 90 % once a released note has faded;
  - Ladder BP: the ladder's loop around low- and high-pass stages in turn, its feedback added rather than taken off,
    so that it rings at its center: 12 dB (taken after its second stage, 6 dB per octave either side) or 24 dB (after
    its fourth, 12 either side), 6 and 18 playing as the slope above, at unity at its center without resonance. Its
    peak rises with Resonance, its input taken down as it does (by 1 + k/4, as the ladder's is taken up) so that the
    peak stays about as far over what it passes as the ladder's; it saturates in its stages, takes Drive and
    self-oscillates as the ladder does, ringing louder than the ladder, since what it passes at its center stays at
    unity;
  - Diode: a four-pole diode ladder, its stages loading each other (each the one before it half as much as a passive
    RC ladder's would), so its poles spread apart: darker than the ladder without resonance, and its peak climbs
    toward the cutoff as the resonance rises, the cutoff where it lands at the top. 24 dB only. It saturates once, at
    its input less the feedback, loses less of its bass than the ladder (half of the loss put back), takes Drive as the
    ladder does (its resonance coming down as a driven sound grows louder, some 22 dB at full Drive, where it came
    down 9), and self-oscillates from about 93 % in the same way, held at 90 % once a released note has faded;
  - Sallen-Key LP and Sallen-Key HP: a two-pole Sallen-Key filter, as in Korg's MS-20, 12 dB only: soft without
    resonance (two poles at the cutoff), with its resonance's loop through a clipper set low, so that as Resonance
    rises it screams and breaks up rather than ringing clean. It takes Drive at full level, its output brought back, as
    the ladder does, but held under the other types' level: what goes in passes it unsaturated, all but its loop's
    share, so more would only make it louder. It self-oscillates from about 93 % in the same way, held at 90 % once a
    released note has faded;
  - Formant: three band-passes at a vowel's first three formants (a bass voice's A, E, I, O and U), each at its level.
    **Vowel** (A to begin with) goes from one vowel to the next in equal steps, moving the formants on a log scale and
    their levels in dB, so it can be swept or modulated like any knob. Cutoff shifts the formants, half an octave for
    each octave it moves from its middle (where they are as the table has them), and Key Track with it; Resonance
    narrows them, from a Q of 4 to 30;
  - Morph: the state-variable filter going from its low-pass to its high-pass as **Morph** goes from 0 to 1
    (0 to begin with), through its band-pass or its notch as **Morph Mode** says (LP > BP > HP, the first, or
    LP > Notch > HP, its notch switch on). **Slope** picks 12 or 24 dB, as a band-pass's or notch's does (6 and 18
    playing as the slope above): at 24 a Butterworth stage after the resonant one, shared out the same way. Its outputs
    are shared out: through the band-pass, the low-pass gives way to the band-pass over
    the first half and the band-pass to the high-pass over the second; through the notch, the high-pass rises to meet
    the low-pass (the two together the notch) and the low-pass then falls away. At 0, 0.5 and 1 it plays exactly the
    low-pass, the band-pass or notch and the high-pass at its slope, with Resonance as theirs; Morph can be swept or
    modulated like any knob.

  Every filter's Cutoff, the filter effect's too, starts (and resets) at its knob's middle, about 632 Hz: there,
  a bipolar note source on it at 100 % puts it right on the note. Each filter also has Resonance, Key Track (how
  far the cutoff follows the note from C4 on, an octave per octave at 100 %; 0 to begin with, on every filter) and
  Drive (saturation before the filter, anti-aliased like the shapers, its output brought back to the level the sound
  went in at: two followers of the power, of what goes in and of what the saturator puts out, averaged over 50 ms,
  give the gain back, so a quiet sound and a loud one keep their levels at any Drive, within a dB once settled, and
  Drive changes the tone alone; until 0.17 its output was divided by the drive's square root, taking a full-level
  operator 5 dB down at full Drive and putting a quiet one 9 dB up), and **Mix**, which blends what goes in (before
  Drive) with what the filter makes of it, as the filter effect's does (100 % to begin with, all filter): everything
  the filter puts out, to the output, the operators, the filters and the shapers, is that blend. A voice only ends once its filters have
  rung out (a plucked comb through the silences between its pulses too), and a filter whose sends go to 0 rings
  out too; when they come back, or its type or slope takes up a stage it left out, that starts from silence.
- **Shapers S1 and S2**, per voice, each switchable on and off (off to begin with): the shaper effect in every
  voice, with its curves (Hard, Soft, Asymmetric, Cubic, Gentle, Sine Fold, Fold, Stack, or a Custom curve drawn
  like the effect's, the S it starts as included), its Drive (0–36 dB), Output, Mix, Link (on to begin with),
  Symmetric (off) and, for the Stack curve, Stack (1 to begin with), and run by the same code: anti-aliased the same way, a drawn curve gliding to moved points and its level at the middle taken out the same way, with the same DC blocker after the uneven and drawn curves. Any operator,
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
- **Modulation**: up to 32 sources, any mix of LFOs (sine, triangle, saw or square; 0.02–100 Hz,
  and at the Rate knob's bottom 0 Hz, where an LFO stands still at its Phase), mod envelopes (AHDSR, starting at full sustain and with an operator's 15 ms release; times as an
  operator's envelope has them), Curve sources (LFOs of a drawn shape), and two that follow what a note or the
  controller brings:
  - **Curve**: an LFO of a shape drawn from points (see below), with everything an LFO has (Rate or a synced note
    value, the clock modes, anchoring, Phase) but the shape list, and a loop region and a fifth clock mode,
    **Sustain**, of its own (see below). It starts unipolar, where an LFO starts bipolar.
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
  name in the Mods view (see the editor): LFOs, Random sources, the pitch bend and the operators start bipolar, and Curve sources, envelopes, macros,
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
  fade an LFO's vibrato in, or velocity open an envelope's sweep wider. The aux's **depth** (0–100 %, all of it to
  begin with) says how far down the aux can take the modulation: at 100 % an aux at 0 silences it, at 70 % it keeps
  30 % of its depth (an aux at half keeps 65 %), and at 0 % the aux does nothing. Taking the aux away puts its depth
  back to 100 %; another aux in its place keeps it. Set either in the Mods view (see the
  editor); the aux also by dropping a source's handle on the modulation's depth handle, or on its aux cell in the Mods view. An aux that runs per voice
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
  A Curve source in Sustain that modulates something keeps the voice running the same way until it has played on to
  its curve's end.
  Once the last voice has ended, its modulation of the effects (and of Volume) runs on while they ring out, so an
  LFO on a filter after a long reverb keeps sweeping the reverb's tail: a Free or Anchor LFO along its shared clock,
  a Retrigger or One-Shot one carrying on from where the voice left it (a Curve in Sustain, let go with its voice,
  playing on to its curve's end), its own params as every voice has them
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
  - **Operators**: each operator's output (**Op A** to **Op F**) is a source too, picked in the Mods view's source and
    aux menus, or dragged from the operator's letter at the top of its page in the inspector, which is its handle and
    lights under the mouse. Like velocity each has 16 slots and no switch of its own, runs per voice and reaches the effects through
    the newest voice. It puts out what the operator plays, after its envelope and whatever its Out says, so an operator
    heard by nothing can modulate; one switched off modulates nothing. It starts bipolar, swinging its targets both
    ways of the set value with the output; unipolar, it takes them one way, from nothing up to as far as the
    operator's envelope has risen, so a silent operator moves nothing either way.
    **Audio rate**: on a param the voice plays by, an operator moves it every sample (at the voices' oversampled rate),
    as it plays, so a fast one gives sidebands as FM does: an operator's Ratio, Pitch, Detune and Freq, Out, Pan,
    Phase, its sends to the filters and shapers, a route's depth (from an operator, filter or shaper into an
    operator), its Unison Detune, Spread and Blend, Warp Amount, its envelope's times, sustain and curves and its
    noise's Slope; a voice filter's Cutoff, Key Track, Resonance, Drive, Vowel, Morph, Out and Mix and its routes to
    the filters and shapers; a voice shaper's Drive, Output, Mix and Out and its routes; and the voice's Pitch, FM
    Amount and Pan (a key-tracked filter following the voice's Pitch as they move it). Each sample takes what the
    operators put out the sample before, so an operator can move its own pitch, level or phase. Its Amount, aux and
    depth are read once per 32-sample block. An operator's table suits the highest pitch the modulation can take it
    to, so moving its pitch stays clear of aliasing as FM does, and a route,
    send or Out at 0 that the modulation opens runs. Every other target (an effect's, a source's own param, a macro's
    knob, a slot's depth and Amount) takes modulation at the control rate, once every 32 samples (an effect gliding
    between, as its other modulation does), from the operator's output averaged over the last such block: a slow
    operator (a fixed one at a few Hz, say) moves it along its wave and envelope, and one far above that rate averages
    away rather than aliasing. So does an operator as an aux. The
    Mods view's rows and the knobs' rings show the average.
    **Filter FM**: on a voice filter's **Cutoff**, the cutoff moves every sample, by as many octaves as the depth takes
    the knob (100 % at full swing being the knob's 10 octaves). A voice's filter follows that voice's own operator. A
    filter effect's Cutoff takes it at audio rate too, the one effect param that does: a filter effect, which hears
    all the voices together, follows the newest voice's operator, as the effects' other modulation does, and takes
    over from one note to the next over about 5 ms, so a new note doesn't click; an operator at a fixed frequency
    moves it alike whichever note is newest. The filter effect's Formant type's cutoff stays with the 32-sample blocks.
  - An LFO runs in one of four clock modes, picked from a list: **Retrigger** (the default), **Free**, **Anchor** or
    **One-Shot**. Retrigger restarts the LFO from its Phase with every note. Each of the other three is a switch
    hosts see; the editor turns the other two off as it picks one. Should a host turn on more than one, One-Shot
    wins, then Anchor. A Random source has only Retrigger and Free (see Random above). A Curve source has a fifth,
    **Sustain** (below), which wins over the other three.
  - **Free** makes an LFO run on without restarting at each note. All voices then follow one shared clock, which
    runs at the rate the newest voice hears (like the effects) and keeps going between notes.
  - **Anchor** ties an LFO to the host's transport: while it plays, the LFO stands where it would had it started at
    its Phase at the very start of the song and run ever since, so it lands on the same spot of its cycle at the same
    place in the song however playback started (a synced LFO counts in beats, so a quarter-note one is at its Phase
    on every beat; a free-running rate counts in seconds). All voices share it, as a free LFO's clock, whenever
    notes start. While the transport is stopped, or in a host that gives no position, it runs on from where it is,
    as a free one, and it lands back in place as playback starts. As its place comes from the song position times
    its rate, modulating the rate moves it along at once rather than speeding it up. A one-shot is never anchored.
    The browser demo's and `fmvibe-uihost`'s transports are stopped, so an anchored LFO runs free there.
  - **Phase** (0–100 %, 0 to begin with) is where in its cycle each note starts an LFO: at 25 % a sine starts at
    its peak, and 100 % is the cycle's start again. A free LFO keeps its shared clock, so it doesn't use it (nor does
    a Random source, which keeps its steps); an anchored one stands at it at the song's start. A one-shot runs from there to the cycle's
    end, so at 50 % it plays only the second half.
  - **Sync** locks an LFO to the host tempo: its rate becomes a note value from 8/1 (a cycle every eight bars) and
    8/1T, then 4/1 down to 1/64, each straight, dotted (D) or as a triplet (T). A Random and a Curve source sync the
    same way; the effects' synced times and sweeps stop at 4/1D. Without a tempo from the host, it assumes 120 BPM.
    The title bar's **H** switched off, every synced LFO, delay and sweep follows the synth's own **tempo** instead
    (see Header). A new LFO
    starts synced, in quarter notes. The note
    value takes modulation like a knob, from the editor's sources, but in steps: it moves from one
    note value to the next along the list (up for faster), never between them, and the dot on its knob stands on
    the one playing.
  - **One-Shot** runs an LFO through one cycle from each note and then holds it where the cycle ends (a one-shot
    is never free or anchored): for a sine, a triangle or a drawn shape that is where it started, while a saw holds
    at its top, a square low and a Random source at the value it took. A drawn shape with a point at the cycle's end
    (100 %) holds at that point's level instead, so a ramp from 0 up to 1 there stays at 1. A Curve source's
    one-shot plays its whole curve whatever its loop region.
  - **Sustain**, a Curve source's alone, plays it as an envelope with a sustain loop: from each note it plays from its
    Phase on into its loop region and round the loop for as long as the key is down, or the sustain pedal holds the
    note. As the key comes up it leaves the loop at once, wherever it is (before the loop or in it), and plays on from
    the level it is at: it goes on from the loop's end, the step between that level and the curve there fading out
    along the way to the next point, so it lands on that point and plays the rest of the curve, then holds at the
    curve's end, as a one-shot does. A loop that ends at the curve's end glides there over 5 ms, and so does one that
    ends on a point the curve drops (or jumps) from straight away, going down with the drop. The note's voice runs
    on until the curve has ended, so what it modulates follows it down. In mono a new key struck while it plays on
    starts it again from its Phase (legato, a key played over a held one leaves it looping where it is). It is the
    last of a Curve card's clock modes (see below).
  - **Unipolar** makes an LFO swing from 0 to 1 instead of −1 to 1, so it only pushes its targets one way, like an
    envelope. It is the same choice the LFO's right-click menu offers, and its display then has its zero line at the
    bottom.
  - A Curve source's shape is drawn from 2 to 64 points, starting as a triangle: up from the bottom to the top at the
    middle of the cycle and down again. Each segment between two points can be bent, from easing in
    to easing out, and the last segment wraps around to the first point, so the cycle loops without a jump. The
    points are saved with the patch (as params of the editor's own, which hosts aren't given: see
    [CLAP and VST3](#clap-and-vst3)).
  - Every Curve source has a **loop region**, a start and an end along its curve, the whole curve to begin with (so
    a patch from before plays as it did). Retrigger plays from its Phase on into the loop, then round the loop alone (a
    Phase past the loop plays on to the curve's end first); Free and Anchor cycle the loop alone, an anchored one
    counting the loop's cycles from the song's start; One-Shot ignores it; Sustain loops it while the key is down.
    Rate (or the synced note value) stays the whole curve's, so the loop plays at the speed it is drawn: a loop half
    the curve long comes round twice as often. A loop of no length, which only a host or a saved state can make,
    holds where it is. Its ends are saved with the patch, as params of the editor's own that nothing modulates, and
    are set on the loop's bar over the shape, or from a point's right-click menu (see the shape editor).
  - A Curve source's **Slew** (Off to begin with, up to 1 s) lags what it puts out behind its curve, from where the
    curve is as the note starts, as a MIDI source's Slew follows its controller: a jump in the curve, or its loop
    wrapping, glides instead of stepping. Off, it puts out its curve as it always did. A note joining a held one in
    poly legato goes on from where the held one's Slew stands, and the effects' tail, once the last voice has ended,
    from where the voice's stood.
  - In One-Shot and Sustain, unsynced, a Curve source's **Rate** reads as the curve's **length** (1.20 s rather than
    0.83 Hz), on its knob and in its full-size editor, whose label then says Length, and a length typed into it ("1.5
    s", "800 ms") sets it; a rate typed with "Hz" still goes. The param is still the rate, so nothing saved changes.
    At 0 Hz it reads as a rate, as it always did. Its places along the curve (a point's readout, the ruler over its
    full-size editor and along its widened card's plot) read as times whenever it is unsynced and moving, a looping
    one's along a cycle of its Rate; synced, in bars and beats (1.2.1: bar, beat, sixteenth, from the cycle's start);
    standing still at 0 Hz, as a share of the cycle.
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
    slope to fresh ones at the new over 5 ms. **Latency**: the filter effect runs at twice the host rate, the shaper
    effect at its oversampling (twice to begin with) and the amp at its, up and back down through half-band filters, which comes out a few samples late
    (about 5 at 48 kHz for twice the rate) and later still toward the top; a flanger with TZF on comes out 6.5 ms late.
    Where paths meet (a join, a node more than one wire goes into, the output), each path is put through the same
    filters up and down, and the same 6.5 ms waits, as many more times as it lacks against the latest one, so they meet
    in step at every frequency: a split's bands sum back flat whatever runs on them, and a path beside one of these
    effects doesn't comb with it. Such an effect is as late switched off as on (what it passes then, and what it fades
    from, goes up and down or waits too), so switching it moves nothing; a new effect, another oversampling, TZF
    switched or a wire moved that changes how late a path is crossfades that path over 5 ms. A wait started afresh
    (a flanger through zero coming in, its dry sound's new path, a new pad) is silent for its 6.5 ms, longer than the
    fade, so what fades in waits for it to fill and what fades out goes on meanwhile, leaving no gap. The output is
    then as late as its latest path, which, with the voices' decimator's couple of samples, is the latency the plugin
    reports to the host; a change that moves it asks the host to restart the plugin, which takes up the new one (see
    Restarting, under CLAP and VST3). Only what the output hears runs: an effect no wire takes on to the
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
    Custom, and the other curves ignore it. **Stack**, after Fold and before Custom in the list, is a soft curve (tanh) biased off its
    middle, as a tube stage is: it saturates gradually, passes what is quiet at its level, and its lower half reaches
    further than its upper half (to −1.25 and 0.83), which adds even harmonics and an offset that grows as it is
    driven harder. It runs as a gain stage of a pedal does, a high-pass at 20 Hz before the drive like the capacitor
    between two stages, and nothing rounding its top off: each stage lifts back what its anti-aliasing's average takes
    off the top, and the shaper effect runs at twice the host rate (the voices' run at their
    oversampling), so stacked eight high at 0 dB, quiet sines to 15 kHz come out within half a dB of their level. The
    DC blocker follows. Its **Stack** (1–8, 1 to begin with), a stepper without a label right of LINK under the
    display, the two centered together, where it shows only for the Stack curve (on a voice shaper's page, left of
    LINK in the curve list's row, the list narrowing for it), runs
    that many stages in a row, each driven by Drive and brought back down after it by as much as Link would (whether
    Link is on or not), so a stack plays about as that many linked shapers in a row; the DC blocker, Output (linked
    or not) and Mix come once, after the last, as the stages' high-passes take out what the stages before them put
    off the middle. Stacked high, it gets denser rather than louder, and its low end thumps: the offset each stage
    puts out moves the next stage's high-pass, which the stages after it drive, and what the high-passes still hold
    as a note ends rings on a moment past it as a low thump (stacked eight high at full drive, about half a second
    above −60 dB) before it stops (what a high-pass puts out is taken 10⁻⁹ toward 0 before it is driven, so nothing
    far below hearing is brought up for ever). The display draws the stages' curves one after another, with the
    gain between them, without their filters. The curves are anti-aliased (first-order antiderivative), each Stack stage too, and
    the shaper effect runs every curve at twice the host rate, so high drive stays clean and a quiet sound's top comes
    through (a 15 kHz sine within about a dB of a 1 kHz one, where the anti-aliasing's average at the host rate took
    5 dB off it), though folding at the highest drives gets duller for it. Band-limited so, a hard curve's peaks
    overshoot its ceiling a little, as the voices' shapers' do. Its **oversampling**, 2x (to begin with), 4x or 8x, is
    picked in its card's right-click menu (the one in force checked; it isn't on the card): a hard-clipped C7 driven
    18 dB keeps what isn't a harmonic about 58 dB down at 2x, 78 dB at 4x and 89 dB at 8x, for the latency of the
    voices' oversampling's half-band stages at that factor (see Latency). Changed under sound, the shaper ducks out
    over 3 ms, starts the new rate from silence and ducks back in. **Tone** (−100 % to +100 %, 0 to begin with) tilts
    what goes into the curve, up to 15 dB above about 700 Hz either way, and tilts what comes out exactly the other
    way: what the curve leaves alone comes out as it went in, while up drives the top harder (a brighter, fizzier
    distortion on bright sounds, and a softer one on a bass, whose harmonics the tilt after takes down) and down
    drives the lows harder. Its knob comes after Drive.
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
    down through the same half-band filters as the voices, a few samples late (see Latency, under the effects): at 4x,
    Heavy at full Gain and Volume on a
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
    time; and Mix, where the dry signal stays at full level up to 50 % and the echoes reach full level there. On the
    card the time knobs read Time L and Time R, synced Rate L and Rate R, and linked (the left one setting both
    sides) Time or Rate alone, their values showing under the mouse as other knobs' do. Time
    changes glide like tape rather than click, read between samples on a cubic as they do, and the times land on whole
    samples, so the repeats lose nothing of their top to the reads (only Tone darkens them). **Duck** (0 to begin with,
    between Tone and Width) turns the echoes heard down while what goes in is loud, as far as it says times how loud
    that is against −12 dBFS (at full Duck an input that loud takes them away), following it up over 5 ms and down
    over 250 ms; the echoes going round stay as they are, so they come back up as the playing stops. The plugin keeps
    processing until the echoes have died away.
  - **DJ EQ**: a three-band isolator with Linkwitz-Riley crossovers (24 dB/octave) at 250 Hz and 2.5 kHz. Each
    band goes from kill to +6 dB, and with all three at 0 dB it is flat.
  - **Param EQ**: three bands, each with Frequency (20 Hz–20 kHz), Gain (±18 dB) and Q (0.3–10), a shape:
    Peak (a bell), Low Shelf, High Shelf, Low Cut or High Cut (12 dB per octave, Q setting its resonance; the
    gain does nothing there) or Notch (taking its frequency away, Q setting how narrowly; the gain does nothing
    there either), and an **On** switch. All three start on, flat (at 250 Hz, 1 kHz and 4 kHz). A band switched off leaves the sound
    alone, fading out and back in over 5 ms. **Amount** (100 % to begin with) mixes what the bands put out with
    what goes in, as a Mix knob does: at 0 % the EQ leaves the sound alone. The bands sound as their display draws
    them in the top octaves too, within about a dB up to 15 kHz: their poles and zeros go where the analog band's are
    (the high cut's zeros matching its level at 0 Hz, its corner and the half rate instead), where the usual bilinear
    design squeezed a bell or shelf up there narrower and lower than drawn.
  - **Filter**: the voice filters' types (their families: low-pass, high-pass, band-pass, notch, comb, formant and
    morph, in a list across its card under its title, as the shaper's curves are; with the list of slopes under the
    response, a morph's with its notch switch (the notch's icon) beside it, a low-, high- or band-pass's with its
    Model list before it, its button naming the model short (SVF, LAD, DIO, SK) and without arrows, its list naming
    them in full; and a formant's Vowel or a morph's Morph between Drive and Mix) and their
    slopes, 6 to 24 dB, on
    the whole mix, with Cutoff, Resonance, **Key Track**, Drive and Mix. Key Track (0 to begin with) moves the
    cutoff with the newest voice's pitch, from C4 on, as a voice filter's does; once the notes end, it stays where
    the last one left it. Like the Param EQ, its display shows what it puts out behind its response. It runs at
    twice the host rate, as the voice filters run oversampled, so the analog types' saturation and Drive alias far
    less and Drive keeps the top. Its ladder and diode take Drive as a voice filter's do: the effects hear the voices
    12 dB down, and a full-level saw's resonance comes down 18 to 22 dB at full Drive, where it came down 14.
  - **Compressor**: stereo-linked and peak-sensing, with Threshold (−60–0 dB, **Thresh** on the card), Ratio
    (1:1–20:1), a soft Knee (0–24 dB), Attack (1–500 ms), Release (1 ms–2 s), Gain (makeup gain, 0–24 dB) and Mix for
    parallel compression. **SC HP** (Detector HP in the host; Off to begin with, then 20–500 Hz) high-passes what its
    detector hears at 12 dB an octave, not the sound itself, so the bass doesn't pump the rest: a quiet 2 kHz tone
    under a loud 50 Hz one comes out about 17 dB louder with it at 300 Hz. Switched on, it rises from letting
    everything through rather than jumping.
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
    own pace between 0.3 and 1 Hz), so the tail's resonances smear instead of ringing on as a metallic tone; each
    line is read between samples through a first-order all-pass, which passes every frequency at its level, so the
    tail's top lasts as long as Damping says (undamped, the octave around 8 kHz falls 60 dB in nearly the decay time
    the octave around 1 kHz does). The pre-delay lands on whole samples. Each side of the input goes through the pre-delay and its own all-passes, then into half the lines, and the
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
    note. **Low Cut** (Off to begin with, up to 1 kHz) and **High Cut** (letting everything through at its top, to
    begin with; down to 1 kHz) are 12 dB an octave on what goes into it, so a room's early reflections take them too:
    the low end kept out of a long tail, or a dark one; switched on they rise from letting everything through rather
    than jumping. **Diffusion** (100 % to begin with) sets how much the input's all-passes smear what goes in, how much
    the lines mix each time round and how evenly what goes in is spread over them (**Diffuse** on the card), all on
    the square of the knob, so a hit's tail roughens about evenly as it turns down (measured as how far its loudness
    jumps from one 2 ms stretch to the next: about 1.4 dB at 100 %, 1.7 at 75 %, 2.1 at 50 %, 2.8 at 25 % and 3.1 at
    0). At 100 % the lines mix through the Hadamard transform and take what goes in alike, as they always did; lower,
    each stage of the transform turns its pairs by less (down to a tenth of its 45 degrees at 0 %), keeping their
    energy, and what goes in goes more into four of the lines (two a side, their lengths far apart), the others keeping
    70 % of their gain at 0 %, its energy kept. At 0 the all-passes are plain delays, those four lines' first echoes
    stand apart and each line's echoes come back nearly on their own for a while: a grainier tail that fills in over a
    second or so, where at 100 % it is dense within about 100 ms. Held, tonal sounds change little with it, as in any
    reverb; hits and plucks show it. The reverb waits for what the all-passes hold back before it sleeps after
    silence, so a lone click at 0 still rings out. Its card has three rows: Size, Decay and Pre; Damp, Width and Diffuse; and Low Cut, High Cut and Mix, Mix at
    the bottom right as on the other cards. A shimmer's, four to a row, puts Diffuse after Pre and its Pitch and
    Shimmer between Damp and Width. Size (the
    lines from a quarter of their length to all of it, gliding when turned, also when the algorithm changes),
    Decay (0.2–20 s to fall 60 dB), Damp (Damping in the host; the corner, from 20 kHz down to 1 kHz, above which the tail falls five
    times as fast, by as much in every line), Pre (the pre-delay, off to 250 ms), Width and Mix (like the delay's: the dry
    sound stays at full level up to 50 %). The plugin keeps processing until the tail has died away, the
    pre-delay's wait included.
  - **Phaser**: 2–12 first-order all-passes (Stages) whose corner an LFO (Rate, 0–20 Hz; at 0 the sweep stands
    still) sweeps up to 2.5 octaves either way of Center (Depth), with Feedback (either sign) from the last stage
    into the first. **Sync** (off to begin with) makes the LFO go round once per note value of the host tempo
    instead, 4/1D down to 1/64T (an LFO's without its two slowest, 8/1 and 8/1T; a bar to begin with; 120 BPM without a tempo from the host). **Stereo** (off to begin with, under its display) runs the right side's LFO a quarter cycle
    ahead. Mix runs to 100 %, which is half wet: there the notches are deepest, and past it they would only get
    shallower again. Its polarity, **+** or **−** (+ to begin with, at the right under its display), picks whether
    the wet sound is added to the dry one or taken from it: negative turns the wet sound over, and the feedback
    with it, so every notch moves to where a peak was (between the positive ones) and Feedback keeps its sense
    either way. Switched, it fades over 10 ms rather than clicking. **Spread** (0 to begin with, at the end of its first
    row) spaces its stages' corners evenly in octaves about where the sweep puts them, up to 3 octaves from the first
    to the last, as an analog phaser's parts spread them: its notches unevenly apart, a less regular, more vocal
    sweep.
  - **Flanger**: a 1–15 ms Delay that an LFO (Rate, 0–20 Hz) sweeps by Depth (from 1 − 0.95 Depth times it to
    1 + 0.95 Depth times it, evenly in octaves, so the notches move as evenly in pitch both ways; its line is read
    between samples on a cubic, so what goes round keeps its top), with Feedback of either sign (a
    negative one gives the hollower sound) and Mix, which runs to 100 %, half wet, like the phaser's (and starts
    there). **Sync** works like the phaser's, two bars a sweep to begin with, and so does **Stereo**, under its
    display, but on to begin with: the right side a quarter cycle ahead. So does its polarity: negative puts a notch
    at 0 Hz and the others where the positive peaks were, the hollow, thin-bottomed negative flange. **TZF** (through
    zero, off to begin with, a switch between Feedback and Mix) delays the dry sound by a fixed 6.5 ms, the most the
    sweep's middle can need, and reads the swept copy as much later less the sweep's middle, so it passes through the
    dry sound twice a cycle: negative and at the middle, every frequency cancels for a moment, the jet-plane sweep of
    two tapes. Its feedback still goes round the swept delay alone. Its output then comes out 6.5 ms late whatever its
    knobs say (turning Delay or Depth moves only the swept copy), which counts in the effects' latency (see Latency,
    under the effects graph): on or off, so switching the flanger moves nothing, while switching TZF moves the
    latency, as a shaper's oversampling does. Switched, the sound fades between the two over 10 ms. Delay and Depth
    moving where it reads slowly (bending its pitch by up to 1 %), it follows them; moving faster, as they jump, it
    crossfades over 10 ms from where it reads to where they now put it (the swept copy, and through zero), rather than
    clicking, as the chorus does.
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
    voice between the sides over 10 ms too. Delay and Depth moving where the voices read slowly (bending their pitch
    by up to 1 %), the voices follow them; moving faster, as they jump, they crossfade over 10 ms from where they read
    to where they now put them, rather than clicking: a Random source of a voice on Delay or Depth jumps that way as
    each new note takes the effects over (they follow the newest voice). Another jump during a crossfade waits for it
    to end. A host or modulation can set 5
    voices, which the card doesn't offer.
  - **Phase Disp** (phase disperser): 1–64 second-order all-passes in a row (Stages) at Freq, each as narrow as
    Pinch sets (Q 0.3 to 6): the sound around Freq comes out later than the rest, smeared in time into a chirp,
    but at full level; Mix blends it with the dry sound. A new Stages fades in from the count before over 10 ms
    rather than clicking (another change meanwhile waiting for the fade to end), so Stages can be modulated too.
    **Key Track** (0 to begin with) moves Freq with the newest voice's pitch from C4 on, as the filter's does: at
    100 %, a C5 puts it an octave up, so a dispersed bass or kick zaps at the same place in each note. **Stretch**
    (1 to 64, 1 to begin with) makes each all-pass's delays that many samples long (at 48 kHz; as long a time at other
    rates), so its dispersion comes again every 48 kHz / Stretch, either way of each multiple: at 32, a chain at
    400 Hz also disperses around 1.1, 1.9, 2.6 kHz and on, each copy chirping longer than an unstretched chain at
    Freq does (at 30, a chain at 600 Hz comes out up to 39 ms late where unstretched it peaks at 12 ms), the metallic
    drip of a spring. Freq stays one of the copies (pulled a little toward where two copies meet, close to
    one). A new Stretch can't take over from the old one without a jump, so the effect fades out to what goes in over
    5 ms and back in at the new length over 4 ms a step of it (20 ms to a quarter of a second): a stretched chain
    starting from silence buzzes for a moment at the copies' spacing, as it does on any note, which the slow fade keeps
    down. Its card has two rows: Freq, Stages and Pinch over Key Track, Stretch and Mix.
  - **Widener** (a dimension expander): the sound above **Cutoff** (a high-pass, 20 Hz–2 kHz, 150 Hz to begin with, 12 dB
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
    above) 65 dB down or more from 50 Hz up; its chains are worked out for the sample rate, longer at higher ones,
    so it holds at 96 and 192 kHz too. A partial shifted past 0 Hz or past half the sample rate is gone rather than
    reflected back the other way, so a spiral of Feedback runs out at either end.
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
  - **Lo-Fi** (under DRIVE in the list of effects): an early digital converter. **Bits** (1–16, 8 to begin with) puts
    each sample on the nearest of that many bits' levels (2 to the Bits), each in the middle of its share of full
    scale, so 1 bit plays ±0.5 (silence going in for 10 ms, under -120 dB, coming out as silence rather than half a step
    off 0); **Rate** (200 Hz–20 kHz, 12 kHz to begin
    with) holds each side between samples taken that often, the steps between folding the top back down as such
    converters did; **Smooth** (50 % to begin with) blends in what a 12 dB low-pass just under half Rate makes of it,
    as a converter's output filter does; **Mix** blends it with the dry sound. Its display draws two cycles of a sine
    and the steps it makes of them, eased as Smooth takes their edges off, with the bits and the rate over it.

  The phaser, flanger and chorus keep their feedback from ringing louder: what they send round is scaled so their level
  across the spectrum stays where it was without it. Effect knobs can be modulated like any other. Their levels and
  mixes ramp to a new setting sample by sample, however the host splits the block, and so do the frequencies their
  filters are set to (the phase disperser's Freq and Pinch, the filter effect's cutoff and its other knobs, as a voice
  filter's do, the Param EQ's bands, the phaser's Center and Depth, the widener's Cutoff): an LFO sweeping one glides
  it rather than stepping it every 32 samples, which buzzed at the steps' rate. As the effects are shared by all voices, the LFOs and envelopes
  reach them through the most recently played voice. Each of the 16 places in the chain has the parameters of
  every effect type. Hosts aren't given the effects' parameters: an effect is set and automated from the editor's
  sources only, and has no remote control page.
- **Global**: **Volume**, the level of the whole output after the effects (−60 to +12 dB, −12 to begin with, room
  for chords and stacked operators; at 0 dB a full-scale operator comes out at full scale. The voices go into the
  effects at a level of their own, 12 dB down, so the effects' drive and thresholds don't depend on it, and Volume
  gives those 12 dB back. A project or preset saved by FMVibe 0.1, whose Volume didn't, loads it 12 dB lower, so
  it sounds as loud as it did. A DC blocker, a 5 Hz high-pass of two poles, takes the DC out of the voices' sum
  going into the effects: a pulse squeezed off 50 %, a Ring route at unison, strong feedback or Exp FM would
  otherwise bias the effects' compressors and drive, build up in their feedback and take headroom at the output.
  (The effects that make DC of their own, the shapers and the amp, block it themselves.) It is cleared while no
  voice sounds, so a choke or the last note's end is silence at once. Hosts modulate it globally, and LFOs and envelopes through the newest voice, as they
  do effect knobs), FM Amount, Pitch (±48 semitones, handy as an envelope target for drums), Pan, **Bend** Range for
  MIDI pitch bend (its knob in the header, after Pitch; a setting, which no source or host modulates), **Mod Wheel**, which moves what is assigned to it (nothing to begin with), **Pitch Bend**
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
- **Microtuning (MTS-ESP)**, off to begin with (switched on in the settings dialog): the notes play in the tuning an **MTS-ESP** master plugin (ODDSound's
  MTS-ESP Mini, say) gives every client in the session, or, while none is connected, the one **MTS SysEx** (the MIDI
  Tuning Standard's bulk dumps, single-note and scale/octave messages) sent to the plugin last; with neither, 12-TET.
  Each note's key plays as far off 12-TET as the tuning says, and its glide, bend, note expressions and Pitch go on
  top, so key tracking and the note source follow the tuning; a fixed operator doesn't. With **Retune** on
  **Continuous** (to begin with), a held note follows the tuning as it changes, at once, a glide going on from there;
  on **Note On**, it keeps the tuning it started in. A key the master leaves unmapped plays nothing (its end reported
  at once), though in Key and Latch the sequencer still takes it to transpose its pattern. The tuning is asked with the
  note's MIDI channel (so a master's tables for each channel apply), the sequencer's and the editor's own notes with
  the first, but every note with none while MPE is on. Switched off, a master and MTS SysEx are ignored. Both are
  settings of the instance, in the settings dialog (see Settings): a project keeps them (one saved before them has
  MTS-ESP off, retuning continuously), presets don't, and undo leaves them alone. The title bar shows a tuning fork
  while a master or the patch's own tuning tunes the synth (see Header). Each instance registers with MTS-ESP as it is made, through ODDSound's client library,
  which finds the master's library (`LIBMTS.dll` in `Common Files\MTS-ESP`, `libMTS.so` in `/usr/local/lib`) where
  the master installed it; without it, MTS SysEx still tunes the synth. The CLAP plugin takes SysEx as CLAP's SysEx
  events, the VST3 plugin as SysEx data events. The browser demo has no MTS-ESP: its settings show TUNING disabled.
- **Tuning files**: a patch may have a tuning of its own, which presets and projects keep with it, loaded from the
  overflow menu (see Header): **Load custom tuning...** takes a Scala scale (`.scl`), Ableton's Scala (`.ascl`, its
  `@ABL REFERENCE_PITCH` setting the pitch of a degree in an octave, octave 4 holding middle C) or an AnaMark tuning
  (`.tun`, each key's cents over its base frequency, `[Exact Tuning]` over `[Tuning]`), of up to 128 notes;
  **Load keyboard mapping...** a Scala keyboard mapping (`.kbm`: which degree each key plays, its keys marked `x`
  unmapped, and the key at its reference frequency), over the patch's scale or 12-TET. With no mapping, a scale's
  degree 0 is on middle C at its 12-TET pitch (as Scala has it), an `.ascl`'s where its reference puts it, a `.tun`'s
  keys where it puts them. While there is one, a faint, indented line under each entry names the file loaded, and a
  reset button at the entry's right end (Init's arrow, its tooltip **Reset tuning** or **Reset keyboard mapping**)
  takes it out; each load and reset is one undo step. The patch's tuning plays
  under a connected MTS-ESP master (which tunes the whole project) and over MTS SysEx, the rules above applying alike:
  Retune, unmapped keys, the channels. A file that isn't what its extension says loads nothing, a message saying
  why. The browser demo plays a preset's tuning, but loads no files.
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
- **Sequencer**, off to begin with: a piano roll that plays the synth's own notes, edited in the Seq view (see
  [Editor](#editor)). It holds four **patterns**, A to D, saved with the preset, each up to 8 bars of 16 steps and up
  to 128 notes; the pattern picked plays, and another one picked while it plays takes over where the loop comes round.
  Each note has its start and length in steps (anywhere, not only on the grid), its pitch, velocity, **chance** (how
  likely it plays each time round, drawn the same way each time a step comes round again, so a render plays alike),
  **Slide** (it holds on into the next note, a step at most, so mono legato and glide join them; a next note starting
  before its end leaves it ending there, as written) and **Mute** (kept,
  silent). A pattern's **loop**, from step to step within its length (all of it to begin with), is what plays. The
  loop moved or resized while it plays, it plays on from the step it is at, going the same way (where that step is
  now outside the loop, from where it falls in it, as many steps from the loop's start as the cycle had it), rather
  than where the new loop's cycles counted from the song's start or the key would put it, and the notes playing move
  with it rather than being chased again (`Sequencer::keepPlace()`, whole steps, an even number of them where the loop's
  length allows, so Swing's pairs stay); with the host's transport it locks to the bars again once the transport
  starts or jumps. Random order follows no place in the loop and goes on picking as it was.
  - **Play** (Key to begin with; a patch saved at state version 16 or earlier that doesn't say loads Host, which it was to
    begin with then): **Host** plays with the host's transport, locked to its bars (and silent while it is stopped; keys still
    play the synth on top). The browser demo's and `fmvibe-uihost`'s transports are stopped, so Host plays nothing there
    by itself; but the browser demo presses the Play button as it loads a patch whose sequencer is on and set to Host, so
    the pattern plays from its loop's start as it loads, with no key held (and stops what the button played for any
    other patch). Each block it plays on from where the last one ended while the host's position is within
    a thousandth of a step of it (a position rounds, one handed in seconds most of all), so a
    note starting where a block does starts once, neither missed nor played twice; further off, the transport has
    jumped, and the notes playing end and it starts over there. **Key** plays while a key is held, from the pattern's start, transposed by the newest key
    from C4 (`kSeqTransposeFrom`: C4 plays it as written, whatever the scale's root), and stops when the keys are let
    go (patches saved before, state version 16 and earlier, transposed it from the root: one playing from a key with
    its root off C4 loads that difference into Transpose, and Octave what Transpose can't hold, playing as it did); **Latch** starts with a key and plays on once they are let go, a
    new key transposing it, until All Notes Off or All Sound Off (MIDI CC 123 or 120, as a host sends as it stops). In Key and Latch the keys play no notes of their own: the host is told each one has ended
    as it is let go.
  - **The Play button** on the SEQ card plays it whatever Play says (`Sequencer::preview()`, through
    `Controller::playSeqPreview()` and the audio thread's key queue, the telemetry's `seqPreview` saying whether it
    plays): from the loop's start on a clock of its own at the tempo, as written but for Octave and Transpose (no key
    transposing it), until it is clicked again, the sequencer is switched off or the plugin is reset. With notes picked
    in the roll, it starts at the earliest one's start within the loop (`SeqPanel::seqPreviewFrom()`; picked notes
    outside the loop are passed over, and with none inside it, from the loop's start): its clock starts where its
    cycle plays that step, going the way the Order goes (Random order from the loop's start), with that step's Swing
    delay taken off so the note sounds at once, and no note from before it chased. Meanwhile the
    keys play notes as they would without the sequencer (in Key and Latch, those it held as it started are let go, the
    host told they have ended), and the host's transport leaves it be. Stopped, it plays as Play says again: in Host,
    from the transport's place, chasing; in Key and Latch, once a key is pressed. It does nothing while the sequencer is
    off. It is the editor's, not a param: hosts don't see it, and presets and the host's state don't keep it. The
    title bar shows it again at the tempo's right while the sequencer is on (see Header, below).
  - **Speed** sets a step's length as a multiple of a 1/16 step's speed, at the host's tempo (the roll's snap names
    its grids for a step of a 1/16): ×1/4, ×1/3, ×1/2, ×2/3, ×3/4, ×1 (to begin with), ×1.5 or ×2, a step of a 1/4,
    1/8 dotted, 1/8, 1/16 dotted, 1/8 triplet, 1/16, 1/16 triplet or 1/32. (Its param, `SeqParam::Rate`, kept its id and
    its values: only its name and text changed, from the note values.) **Swing** (50–75 %) delays every other step; **Gate** (5–200 %) scales every note's
    length; **Velocity** scales the notes' velocities (at 0 % every note plays at 100); **Humanize** delays the notes by
    up to a quarter of a step and moves their velocity up to 30 % either way, the same way every time round;
    **Octave** (±3) and **Transpose** (±24 semitones) move the whole pattern. **Order** runs the loop's steps forward,
    backward, to and fro (the first and last steps play twice at each turn, so a loop of 16 steps takes 32 and stays
    in its meter), or at random among the steps notes start in (`Sequencer::randomStep()`): from a step a note
    starts in it plays on to the next such step (round the loop's end to the first), then picks another, any of them,
    repeats too, so the notes come in a random order each in time and the playhead moves on steadily between the
    jumps; each time round the loop it picks again from the loop's start, the last pick cut short at the loop's end so
    the loop keeps its length and its bars, and the same picks come for the same place each time it plays (with no
    note in the loop it runs as Forward; muted notes are left out; a pattern read again as it plays chases no note
    from before, as the picks may land elsewhere then), the notes starting in a step at the
    same place in it.
  - **Curves**: each pattern also has a **bend** curve, a **mod wheel** curve and a curve for each of the eight
    **macros**, drawn in the lane under the roll:
    up to 64 points each, at any step, each segment straight or bent. As the pattern plays, the bend curve (±1, a share
    of Bend Range) adds to the pitch bend, on top of the bend wheel's and the host's, and the mod wheel curve (0–1) to
    the Mod Wheel param, as the host's modulation of it would, moving what the mod wheel modulates. A macro's curve (0–1)
    takes the place of its knob's position, wherever the knob is set, what modulates the knob still adding to it. As a Curve source's last
    segment does, a curve's last segment wraps around: from its last point on past the pattern's end and in from the
    pattern's start to its first point, so the pattern comes round without a jump (bent by the last point's bend, with
    its handle at its middle, on whichever side that falls). A point past the pattern's end, left by a pattern made
    shorter, is left out until the pattern reaches it again. Played backward, a curve runs back with the playhead. A curve without points does nothing, nor does any while the sequencer doesn't play. The wheels in the
    keyboard strip and the macros' knobs show where the curves take them as they play and go back to their params as they stop; the curves edit no
    param, so nothing of theirs reaches the host or the undo history.
  - **Note bends**: each note may also bend its own pitch, as an MPE controller bends one finger's note: up to 8
    points, each at a time in steps from the note's start and a pitch in semitones (±24), each segment straight or
    bent, as the curves' are. Before its first point the note plays at that point's pitch (so it can start low and
    scoop up), and after its last at the last's. The bend moves only the voice playing that note, on top of its glide,
    the bend curve, the bend wheel, the host's bend and Pitch, and isn't scaled by Bend Range; Octave, Transpose and a
    key move the note and its bend with it. It runs in steps from where the note starts (after Swing and Humanize),
    smoothly rather than once a stretch, and isn't scaled by Gate: a note Gate shortens holds the pitch it reached at
    its end through its release. Reverse and Ping-Pong play it forward, as they play the notes. A note chased in its
    middle starts where its bend is there. A note gliding from a bent one (in mono or poly) starts its glide from the
    pitch that one was bent to, so a bend up into a slide to the note it reached joins without a jump. The
    note source and key-tracked filters follow the bent pitch; fixed operators don't move. The SEQ view draws them over
    the roll and edits them with its **Bend** tool (see [Editor](#editor)).
  - Its notes start and end at their sample, however the host splits its blocks, and go to the voices as notes do
    (stealing, mono, legato and glide alike); the host is never told of their ends, and the keyboard strip lights only
    the keys played (by the host, its own keys and the computer's), not the sequencer's notes. A jump of the transport, or a
    change of Rate while it plays, ends what it holds and plays on from there.
  - It **chases** notes: as it starts playing (the host's transport starting, the transport jumping, or it switched
    on as the transport plays) and whenever the pattern is read again (a note put down, moved or lengthened, or a
    setting changed, while it plays), a note the playhead is in the middle of that isn't sounding starts at once, for
    what is left of it, at its velocity. Not one its chance leaves out, nor one from before where it started: from a
    key it starts at the pattern's start, with the host from the song's start, and another pattern taking over as
    the loop comes round chases none of its notes ringing across the loop's end from before.
  - While it may start notes (in Host while
    it is on; in Key and Latch while it runs; while the Play button plays it), the plugin doesn't let the host put
    processing to sleep.
  - Its settings but Root and Scale are given to hosts, for automation (not modulation); a pattern's length, loop,
    notes, their bends and curves are the editor's alone.
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
  out the ones past it at once, as stealing does. Raising it past the voices allocated takes a restart (see
  Restarting, under CLAP and VST3), which waits until the limit has settled: while the Voices knob is held, and then
  until it (or a host stepping the param) has left it alone for half a second, so a drag or a run of steps asks once,
  for where it stops; any other restart needed meanwhile (a new latency, another oversampling) waits with it, so
  they come as one. A project or a preset loaded asks at once. Voices are stereo all the way through the filters.
  Voices without any unison spread run their filters once, so they cost no more than mono voices.
- **Oversampling**: the voices (operators, shapers, filters and their drive) run at 1, 2, 4 or 8 times the host rate, 2 by
  default, and come back down through a polyphase IIR half-band decimator: flat to 20 kHz, about 2 samples late at
  2x (the plugin reports it to the host, see Latency). This keeps bright FM, strong feedback and filter drive from
  aliasing. The effects run at the host rate, but for the shaper and filter effects (twice it) and the amp (its
  oversampling). Levels, sends and
  routing amounts ramp sample by sample, and the ramps don't depend on how the host splits the block.

An operator or filter that is switched off is silent and costs no CPU. It is left out of the routing graph and
greyed out in the matrix, but its settings and connections are kept for when it comes back. A new instance starts with only operator A switched on,
as a plain sine carrier; the other operators share A's defaults (except that they don't go to the output), so
switching one on and wiring it in is the whole job.

## Editor

The editor is drawn with Direct2D and scales freely: hosts can resize it, and it keeps its aspect ratio. A window dragged by
its side takes its width and the height follows; dragged by its top or bottom edge, it takes its height and the width
follows (`Editor::constrain()`: whichever side the host moved further from the editor's size). Collapsing
the keyboard strip makes it shorter (the plugin asks the host to resize the window). It keeps
its size as it is scaled on the monitor its window is on: in a host aware of each monitor's scaling, opened on (or
moved to) one scaled otherwise than the main one, it asks the host for the size at that scaling. A size the plugin opens
at or asks for is held within the monitor's work area: a project's or the last editor's size that no longer fits, and
the taller view a fold unfolds to, are scaled down to fit, the zoom kept, so opening the same project on a larger
monitor shows it at that size again. While
only what keeps to its place moves (the output meter and scope and the playing count in the header, the keys, an
operator's scope in the inspector), it repaints that place alone. So it does as notes play: a source moving repaints
its card, and what its modulation moves the part of the editor that shows it (the header for its knobs, the panels'
row for an operator's, a filter's, a shaper's or an effect's params, a source's card for that source's own, and the
panels' row too while the Mods view shows); an operator's envelope repaints the inspector, and the effects' meters,
sweeps and spectra the panels' row. What may show anywhere repaints the whole of it: the note's pitch and velocity
changing, the first note or the last ending, and the host's tempo. It looks over the params for changes only when
one may have changed since it last did.

Its buttons look as what they do, wherever they are. An action (undo and redo, the preset buttons, the **...**, the bins,
the Seq view's copy and stretch, a shape's tools) is an icon alone, named in its tooltip, on a raised face; the small
ones in a card's title row, its fold and its x, show that face only under the mouse itself, as tall as the power switch
beside them, and the fold shows at all only while the mouse is over the card (an effect's, a source's or a split's
container, the effects in its bands counting as over it, and from a finger's first touch on, always), still taking a click while hidden. A switch or a choice of a param is set in a well and lit in its color, a row of them all icons or all words. A
button that opens a list or a menu shows its name, after an icon where it has one, then a chevron pointing down, one
chevron alike on every such button (a smaller one inside a row). A tab is its name, the one shown marked. A card's one
main action (the Seq card's Play) is its icon and word in the accent, on the accent's tint, lit as a switch is while
what it started goes on.

- **Routing / Matrix / FX / Seq / Mods** tabs, under the header, pick what the panel below them shows: the routing
  graph or the matrix (two views of the same connections) with the inspector floating over the panel's left end, or
  across the whole panel the effects chain, the sequencer or every modulation. The sequencer's view, at its full size,
  reaches down over the modulation panel too, as tall as the two together, the source cards coming back with another
  view; at its half size it covers the panel alone, as the FX view does. Its card's size button and a double-click on
  its tab while it shows switch between them (a double-click whose first click shows it leaves it); the size is the
  user's setting, kept in their settings file (full to begin with), not the patch's, and nothing to undo. The
  choice is saved with the project, but loading a preset or Init leaves the tab shown (and the FX view's row or graph)
  as it is. They are folder tabs: the one shown is part of the panel under it, its outline
  curving into the panel's edge (Routing, in line with the panel's left edge, runs straight on down it), with the
  theme's accent along its top; the others are their names, a tab's shape
  showing faintly under the mouse. FX shows a dot in its upper right while an effect in the line is switched on,
  and Seq while the sequencer is on. Tabs mark their upper right one of two ways, alike everywhere: a filled dot
  says what the tab shows is at work and heard (these two, and a node's tab in the inspector), and a ring, as a
  modulated knob has, says a control on the tab, shown or hidden, is modulated (an operator page's tabs). While the full-size shape, shaper curve or partial editor has the panels, no tab
  is shown, and clicking one closes the editor (a widened LFO card's shape stays in its card).
- **Seq** (the sequencer, see [Sound engine](#sound-engine)): its settings on a card where the inspector floats, and
  the roll of the pattern picked in the rest, at its full size down to the modulation panel's bottom, which it covers,
  with the patterns' overview under the card; at its half size, down to the panels' row's bottom, without it.
  - **The card**: the sequencer's power switch, and left of it the size button (the Curve card's expand button at
    the half size, its collapse button at the full one, its tooltip saying which); **Pattern** A to D (the one picked plays and shows in the roll; the
    overview's tiles copy and paste them), a modulation target too, in whole steps: a source's handle dropped on its
    buttons (ringed together as one while carried) modulates it, the Mods view's target menu lists it under Sequencer,
    a host may modulate it, and the button modulation picks, while it isn't the one set, is ringed, as the dot on its
    reach shows it (with nothing playing, where the macros and the mod wheel take it); under the row, as under a knob, its modulations' reach (a line from A's middle to D's, with the set
    pattern's tick and the live dot) and their depth handles, centered, Play sitting lower to leave them room; the sequencer takes a pattern modulation picks as it takes one clicked, at once while nothing plays
    and else where the loop comes round; at the row's right end the **Play** button (see Sequencer), the card's main
    action: a play icon and its word in the accent, on the accent's tint, saying Stop with a stop icon and lit as a
    switch is (filled with the accent in a flat theme) while the telemetry says the pattern plays, a click playing or
    stopping it, editing no param and leaving nothing to undo; **Play** Host | Key | Latch; the
    controls in two rows of four, Speed and Length (the pattern's, in bars) over Octave and Transpose at the left, as
    steppers, and the knobs Swing and Gate over Humanize and Velocity at the right (those four modulation targets, as an effect's knobs
    are: the sources' handles drop on them, the Mods view's target menu lists them under Sequencer, and the host may
    modulate them, for every note at once; the sequencer plays them, before each stretch it plans, as set with the
    host's and the macros' modulation and the newest voice's slots, or the last one's as the effects ring out, and
    the knobs' dots show where it plays them); **Order** as
    arrows (forward, back, to and fro, a die for random); **Scale**: the root (dragged up and down or scrolled, a
    semitone a step, shown as its note name without an octave, since only its pitch class counts; a double-click puts
    it back on C), the scale's list (Chromatic, Major, Minor, Dorian, Phrygian,
    Lydian, Mixolydian, Harmonic Minor, Melodic Minor, Pentatonic Major and Minor, Blues; wider than its button, and
    opening above it where the editor is too short for it below) and **Fold**, which shows only the scale's rows, and
    those of notes off it all the same, their names faint. The rows' ground follows the keys alone, whatever the scale:
    a black key's row darker and a white key's a little lighter, and folded, all of them plain. Notes off the scale are
    hatched across, folded or not. Its switches say what they do in their tooltips (Play's what each mode does).
  - **Switched off**, the whole view but the card's power switch and size button is drawn dimmed, as a switched-off
    node's page is, and takes no input: its widgets are disabled (the mouse passes them by, and the View draws them
    dimmed), what it paints itself is drawn in a layer at the dimmed opacity (the cards' and the roll's faces left as
    they are), and its keys (Ctrl+A, the arrows, Delete, copy and paste), the wheel across, the middle button's and
    two fingers' pan, the roll's and the tiles' right-click menus and a source's handle dropped on PATTERN do nothing,
    the keys going on to the host. A list of its own open as it is switched off (by the host, say) closes.
  - **PATTERNS**, a card like it under it, down to the panel's bottom: a tile a pattern, A to D, one under another,
    its letter at its left (lit, and the tile tinted and ringed, while it is the pattern picked) and its notes drawn
    small after it: across, the longest pattern's length (a shorter one's tile ends short, a line at each bar, outside
    its loop dimmed); up and down, the pattern's own notes, at least an octave of rows, fainter the softer and a muted
    one faintest. The pattern that plays shows the playhead in its tile, and the pattern picked what the roll shows
    of it, as a rectangle: across the steps it shows, up and down its rows (as far as the tile reaches; all the way
    while the pattern has no notes), moving as the roll scrolls and zooms. While the roll has somewhere to scroll to,
    the rectangle (or just round it) drags it there, across and up and down, the cursor moving over it and the
    rectangle brighter under the mouse and while dragged. A click elsewhere on a tile picks its pattern (one undo
    step); a tile carried onto another (half seen, following the mouse) rings that one, and let go of there copies its
    pattern there as the copy button does, length, loop, notes and curves, over what was there (one undo step, and a
    message says so); let go of on its own place or off the tiles, it does nothing. The hand shows over a tile, and
    its tooltip says so. A tile's right-click menu has **Copy Pattern**, **Paste Pattern** (once one is copied) and,
    under a line, **Clear Pattern** (while it holds notes or curve points); Ctrl+C over a tile copies its pattern and
    Ctrl+V over one pastes the pattern copied there. The pattern copied (length, loop, notes and curves) is the
    editor's, kept as patches load. Pasted, or copied by a carried tile, over a pattern that holds notes or curve points
    other than those, it asks first ("Overwrite pattern B?", "Are you sure you want to overwrite this pattern?",
    Cancel | Overwrite); over an empty pattern it doesn't. Clear Pattern asks as the bin does, and clears that pattern
    (its length and loop staying). Each is one undo step.
  - **The grid**: a line at every snap's length across the roll (a step with the snap off), the beats and bars
    stronger, as strong as the settings dialog's **GRID** says (50 % to begin with).
  - **The tools** over the roll: select | draw | bend (select to begin with), **Snap** and its list (Off, 1/4, 1/8, 1/16 to
    begin with, 1/32, 1/64, 1/8 triplet, 1/16 triplet; the wheel steps it), **×2** and **÷2** (the notes picked, or
    every note, twice or half as long and as far apart; the whole pattern twice as long takes twice the bars), the
    field of the notes picked (its Note, Vel, Chance, Len and Offset, the offset a note's start off the grid in
    hundredths of a step, and Bend, the farthest the first one bends, in semitones; each dragged up and down or
    scrolled, moving every note picked, one undo step, Bend scaling each one's bend, its farthest point a semitone a
    step further or nearer, scaled down to nothing taking it away (a drag back bringing it back as it was, as the
    drag scales the bends it started with); a double-click puts Vel back to 100, Chance to full and Offset to 0, on the grid, and
    takes Bend's bends away), **Slide** and
    **Mute** (for the notes picked: on, or off if they all are) and the bin, which clears the pattern, its notes and curves, asking first.
    The roll's settings (the tool, the snap, the lane, the rows scrolled to and Fold) belong to the editor: a preset
    or Init leaves them as they are.
  - **The roll**: the piano keys at its left (white keys across, the black ones shorter over them, every C named; with
    Fold, each row's name), a click playing the key, and the keys the sequencer's notes are written on lit as it plays them (as written, before Octave, Transpose or a key move them), each rising and fading a moment rather than switching; the bars
    and beats over it, with the loop as a bar in the accent (dragged by its ends to size it and by its middle to move
    it, the cursor left and right or moving; across the bars outside it for a new loop; a double-click loops the whole
    pattern again), the steps outside the loop shaded; and the playhead, while the sequencer plays the pattern shown (running backward through the steps it plays
    backward, reversed or on Ping-Pong's way back). The audio thread says where it is once a block of the host's, which
    the editor's polls fall between unevenly; so once the playhead is seen moving on at the tempo's pace, the editor
    carries it on at that pace itself between them, along the loop as the order runs it, easing toward each new
    position and never going back, and shows one that jumps (the host's transport moved, a Random step, the loop
    changed) as it comes. With no new position it goes no further than an eighth of a second's worth past the last. The wheel scrolls the
    rows; over the roll, Ctrl+wheel zooms in and out across and Alt+wheel up and down, at the mouse, as the − and +
    after the arrows at the lane tabs' right do (across from the whole pattern to two steps, the rows from 0.6 to 3
    times as high). A bar at the roll's right scrolls the rows (dragged, a click on its track jumping there, or the
    wheel). Zoomed in across, a bar after the lane tabs shows and scrolls what the roll shows, as Shift+wheel
    and the wheel turned sideways do; the middle button (or two fingers, outside the browser demo) drags the roll
    across and up and down. The
    zoom and what is scrolled across belong to the editor and aren't saved. Notes are brighter as they are louder (at full velocity
    at full strength), dashed with their chance at less than full,
    hollow while muted, a hook off their end with Slide; those picked are ringed, a handle at their end. As the
    sequencer plays, a note glows as the playhead reaches it, fading over as long as the note lasts at the tempo, as
    strongly as the glow setting says (not at all with it off).
  - **Select**: a click picks a note (Shift adds it or takes it out, once let go without a drag), a drag moves the notes picked (the one grabbed
    landing on the grid, up and down by rows; with Shift or Alt held, off it, following the mouse, its Offset saying how
    far off; Ctrl carrying copies), a note's right end dragged sets
    the length of every note picked and its left end their starts, their ends staying (on the grid; with Shift or Alt
    held, off it, following the mouse) (the cursor left-right over
    either end, a handle at each end of a note picked), a drag from an empty place picks the notes the box reaches (Shift adding to
    them), and a double-click puts a note down a grid step long (on a note, deletes it), and held, the mouse up or down
    sets its velocity, as Draw's click does, and to the right or left lengthens it that way (its end or its start), the
    cursor left-right to begin with, then up-down or left-right as the mouse last went (a move of 4 px or more
    turning it). **Draw**: a click puts a note
    down (the cursor a pen), and held, the mouse up or down sets its velocity, a step a pixel from 100; with Shift, a
    drag paints a note in each grid step it crosses that has none; a note clicked is moved or stretched as in Select
    (the cursor saying which over it). Each edit is one undo step. No note is moved, stretched or drawn past the
    pattern's end: a drag or the arrows move the notes picked only as far as every one of them still ends within it,
    an end dragged or a note lengthened as it is drawn stops at it, and a note put down near it is cut short there.
  - **Note bends** (see Sequencer): each bent note's bend is a line from its start along its points to its end, across
    the rows (a row a semitone; folded to the scale, between the rows it falls between, in proportion), lighter than
    the notes and faint behind them unless Bend is picked, a faint ribbon down to the note's own row, dashed for a note muted or at a chance below full, and past
    the note's end (where a note made shorter leaves a point, which doesn't play) faint and dashed. **Bend** edits
    them: the notes go faint and the bends stand out over them, a note without a bend showing its line flat along its
    pitch (which takes the mouse as a bend's line does), every bent note showing its points and the handles at
    the middles of its segments that rise or fall, and the bend of the note under the mouse (or whose line, point or
    handle is) lit, as is the one being edited: a note pressed where its bend runs over its bar (or one without a
    bend) puts a point down where the mouse is and drags it (where the bend has left the bar, the bar takes nothing), on the rows (only the scale's while folded) and the
    snap's lines, within the note (from a point at its start at its pitch, where it had none); with Alt held, or
    Shift while dragging, off the grid, to the cent; a point dragged moves between its neighbors, and a segment's
    middle dragged up or down bends it; the mouse over a bend's line shows a faint point on it where the snap's
    nearest line crosses it (with the snap off, at the mouse), following it along the line, under the arrow, and a
    press there puts a point down there on the line as it runs (with Alt, at the mouse) and drags it; a double-click on a point takes it away (a lone
    point at 0 left going with it)
    and on a segment's middle straightens it, but leaves a point its own first click put down; with Shift, a drag
    across a note draws its bend, a point on every line of the grid it crosses, in place of the points there (pressed
    on its bar, even where its line runs). A note
    bends through 8 points at most (a message says so). While a point is dragged, a readout by it says how far it
    bends, and the note sounds alone at that pitch (in the browser demo too). The note is picked as it is pressed.
    Points are picked as notes are: a point clicked is picked alone (on one of several picked, once it is let go
    unmoved), Shift+click or Ctrl+click picks it along with the others or lets it go (once let go unmoved), a drag
    from empty space draws a box picking the points it holds (with Shift or Ctrl added to those picked), Ctrl+A over
    the roll picks every point and Delete removes those picked, one undo step; picked points are ringed. A picked
    point dragged moves every one picked as far, one undo step, each between its nearest neighbors not picked (or its
    note's ends) and within ±24 semitones. A point put down is picked alone. A double-click on empty space does
    nothing. The cursor moves over a point, goes up and down over a segment's middle or a bar a press bends, stays the
    arrow over a bend's line and a bar its bend has left, and is a pen drawing. Each edit is one undo step. A note's
    bend goes with it: copied, cut, pasted, duplicated and carried as a copy with Ctrl, as the pattern is copied or
    pasted, stretched by ×2 and ÷2, and removed with it; moved or transposed, it bends from where it lands.
  - **The lane** under the roll, a slight line parting the two: a stem for each note at its start, as high as its velocity or its chance, as the
    **Velocity | Chance | Bend | Mod Wheel** tabs under it pick, a note picked ringed there too. After the tabs,
    a **Macro** tab, with a dot in its macro's color (faint while the macro has no curve in this pattern, here and in the
    list) and that macro's number, in two parts lit apart under the mouse: its label, a click on which shows the macro
    it names (the macro last shown), and at its right end its arrow (pointing down, and up while the list is open), a thin line parting the two while either is lit,
    a click on which opens the list of the eight over it ("Choose macro" its tooltip): each with its color, its name and its curve in this pattern drawn small (a faint line
    without one). A row picked shows that macro's curve in the lane, the tab lit as a tab picked is; a click around the list or Escape closes it, and the wheel over the tab steps along the macros. With **Draw**, dragged
    across, the notes there take the height of the mouse (the cursor a pen, and up and down over a dot and while one is
    dragged); with more than one note picked, a drag from one of their dots moves them all up or down together, each
    from where it was. With **Select**, the dots are picked as their notes are in the roll, picking the notes: a drag
    from empty space draws a box that picks the notes whose dots it holds (their stems alone don't count; with Shift or
    Ctrl, it adds them to those picked); a dot clicked picks its note alone, and with Shift or Ctrl along with the others
    (or lets it go, once let go unmoved); a click on one of several picked, let go unmoved, picks it alone; and a dot
    dragged moves every note picked up or down together, each from where it was, one undo step (the cursor up and down
    over a dot, the arrow elsewhere). Either way a double-click puts the notes there back (velocity 100, chance full).
    Where notes start together and any of them is picked, the lane sets the picked ones alone (their dots drawn over
    the others').
    On **Bend**, **Mod Wheel** and **Macro**, the lane shows the pattern's curve instead (see Sequencer), across the pattern as
    the roll is zoomed and scrolled, in the bend's, the mod wheel's or the macro's color, filled down to where it rests (the bend's
    middle, the bottom for the others), with its points and, on each segment that rises or falls and has room, a handle
    at its middle; behind it a grid, the roll's lines running on down through the lane and a line at each quarter of
    the curve's range, as strong as the GRID setting says. As with notes, a point dragged moves between its
    neighbors and a segment's middle dragged up or down bends it, with either tool; with **Select** a double-click
    puts a point down (and on a point deletes it), and with **Draw** a click puts one down and moves it while the
    button is held, and Shift+drag draws the curve, a point on every line of the grid across it crosses (a step apart
    with the snap off), in place of the points there. A double-click straightens a bend (and leaves a point its own
    first click put down). Points go where the mouse is; with **Ctrl** held, onto the grid, across on the snap's lines
    and up and down on the lane's. Each is an undo step. The cursor moves over a point, goes up and down over a bend,
    and is a pen where Draw puts a point down. The whole pattern twice or half as long takes its curves along.
    Points are picked as notes are: with **Select**, a drag from empty space draws a box that picks the points it
    reaches (with Ctrl or Shift added to those picked); Ctrl+click or Shift+click on a point picks it along with the
    others, or lets it go; a click on a point picks it alone (on one of several picked, once it is let go unmoved), and
    with **Draw** a point put down is picked alone. Picked points are ringed in the curve's color. With either tool,
    dragging a picked point moves every one picked as far, one undo step, keeping their spacing: across between the
    nearest points not picked (or the pattern's ends), and up and down within the curve's range. Ctrl+A over the lane
    picks every point, and Delete removes those picked, one undo step. A click off the lane lets them go.
  - **Hearing notes**: a key clicked in the roll's keys (each click, however quick), and a note clicked, drawn or put down by a double-click on
    the roll, sound alone while the button is held (a note moved to another row sounding that row's), whatever Play is: never the sequencer's
    pattern, which only the keyboard strip's keys and the host's notes start in Key and Latch.
  - **Following what is dragged**: a note, a box, a paint stroke, a bend's point (or the points picked) or a stroke drawing a bend dragged to the roll's edge (or past it) scrolls the
    roll that way, faster the further past it, what is dragged going along (a box's start staying on the notes where
    it began, so it reaches notes that didn't show as it started). A patch loaded (a preset, Init, a
    project) fits the roll to its pattern's notes: the whole pattern across, and the rows about the notes, lower where
    they don't all fit.
  - **No two notes on one spot**: a note put down, moved, stretched, quantized or transposed onto another's start and
    pitch takes its place, the one there removed in the same undo step; notes picked that land on one another (quantized
    onto one step, stretched against the pattern's end) leave the first of them.
  - **Picks**: the notes picked are let go of as a patch loads (a preset, Init, the host's state), whose notes are
    another's; notes past the pattern's end (its Length shortened, which keeps them for when it grows again) are
    never picked.
  - **Keys**, while the mouse is over the editor (as for undo): **Ctrl+A** picks every note of the pattern; **Left**
    and **Right** move the notes picked a grid step (a step with the snap off), **Up** and **Down** a row (a degree of
    the scale while folded), and **Shift+Up** and **Shift+Down** an octave, and **Delete** removes them, each an undo step. Moved up or down, the
    notes are heard alone at their new pitches for 300 ms (each pitch once; the next move lets them go), and the
    rows follow only as far as shows every note moved, and not at all while they all show. **Ctrl+C** copies the notes
    picked, **Ctrl+X** cuts them, and **Ctrl+V** pastes what was copied or cut into the pattern shown (any pattern),
    each note's pitch, length, velocity, chance, Slide and Mute as they were: at the step under the mouse on the roll
    (on the grid, the first note there and the rest as far after it as they were), or with the mouse off the roll
    where a cut took them from, or right after what was copied or pasted last (rounded up to the grid), so pasting
    again lays them end to end. The notes pasted are picked, notes past the pattern's end are left out, and each is an
    undo step. **Ctrl+D** duplicates the notes picked: the copies go right after the last of them to end (rounded up to
    the grid), as far apart as they were, and are picked, so pressing it again lays them end to end, and what was copied
    stays as it is. Undoing it picks the notes it was pressed on again, and redoing it the copies. With no note picked, the arrows, Ctrl+C, Ctrl+X and Ctrl+D go on to the host, and so does Ctrl+V with
    nothing copied.
  - **The roll's right-click menu**, on a note (which it picks alone, unless it is picked) or an empty place (keeping
    what is picked): **Cut**, **Copy**, **Paste** (where the menu was opened, on the grid), **Duplicate** (as Ctrl+D does) and **Delete**; **Select All**; **Slide** and
    **Mute** (checked while every note picked has it), and **Quantize**, which puts the notes picked on the snap's
    grid. It offers only what applies: the notes' entries while notes are picked, Paste once something is copied.
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
      steps through them. The pill stays open while the mouse is on it, its list is open or its depth is dragged. A
      label, resting or open, lies over everything around it: it takes the mouse over a node, a wire's end or a depth
      handle, and an open pill over the labels beside it too.
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
    panel's height, as far from its top as from its bottom, its cells one and a half times as wide as they are tall
    and its legend beside them; every cell is a route; the row and
    column of a node that is off are greyed out, its routes kept as they are. The diagonal is each node's feedback,
    outlined in its color; toggled on, it starts at 25 %. A route into an operator that isn't Phase shows its mode's
    glyph in its cell's top left corner, and the cell's right-click menu lists the modes. Drag cells
    vertically; double-click toggles a cell on or off; Ctrl+Click types an amount. Dragging a cell, or toggling it
    on, switches on the nodes it joins. A row's or column's name works like the node's tab in the
    inspector: clicking it shows that node there, switching it on if it is off, and the names of the node shown
    sit on a plate, like its tab, where a click switches it off, or on again.
  - **Algorithms**: **Algorithm**, at the right end of the tabs' row while Routing or Matrix is shown, opens a panel
    of the classic FM algorithms over the graph or matrix (the inspector left in view), in four sets that the switch
    in its title row picks, by how many operators they use (**6 | 4 | 3 | 2**, with "Operators" before them): 6, the
    32 layouts of six, numbered as the classic charts have them; 4, the 8 of four (A to D), as the classic
    four-operator chips number them; 3, the 5 shapes three can take (A to C: a stack, two into one, one into two, a
    pair beside a carrier, all three out); and 2, the classic two-operator pair (B into A, or both out). The feedback of four, three and two
    operators sits on the top one (D, C or B). Each is a
    thumbnail drawn the way the graph runs, modulators on the left, carriers in a column and their lines into OUT on
    the right, each operator in its color and its feedback a small loop on its top (or, for six operators' 4 and 6,
    a dashed curve back up the stack), its number at its top right. The panel's foot names the one under the mouse (its number, how many carriers
    it has and where its feedback is). The algorithm the operators form now is lit, and its number shows on the
    button (with "4-OP", "3-OP" or "2-OP" before it for fewer operators); the panel opens on its set. A click on a thumbnail sets the
    operators to it, as one undo step, and closes the panel; Escape or a click around it closes it too. Picking one:
    - switches on the operators it uses and the others off (their settings and routes kept, as switching off keeps
      them);
    - keeps each route it has as it was (depth, mode and modulation), brings in its others at 50 % (its feedback at
      25 %), and disconnects the routes among its operators that it hasn't, their modulation with them;
    - sends its carriers out: one going out already, straight or through a filter or shaper that goes out, as it was,
      a new one at 100 % / √n for n carriers (71 % for two, 41 % for six); and takes the others off the output, a
      modulator's send into a filter or shaper that goes out disconnected too (what it sends into one that only
      modulates stays);
    - lays its operators out afresh, their wires bowing as new ones do: the carriers in the column where A and D start,
      clear of the shapers and filters nearer OUT, each operator two columns left of the furthest down the stack it
      modulates, the rows spread over the graph's height (past five, every other one a column further left); an
      operator landing on a filter or shaper shown there is moved clear of it, a feedback route back up a stack
      bows round the operators between, and the lines of three carriers or more fan out into OUT, bowing apart so
      their arrowheads come in clear of each other.
    Filters, shapers, their routes and places, the carriers' sends into them and every operator's own sound (wave,
    ratio, envelope, unison) stay as they are. Nothing is stored for it: which algorithm the operators form is read
    from the routes each time (one above 0 % or kept at 0 % counting, whatever its depth).

  - **FX**: takes the place of both the routing panel and the inspector. It shows a card per effect, left to right
    in processing order, each in its type's color (shaper red, delay blue, DJ EQ amber, Param EQ orange, filter
    purple, compressor green, 3-band dynamics teal, reverb indigo, phaser pink, flanger lime, chorus periwinkle,
    frequency shifter cyan, phase disperser slate, widener fuchsia, amp tweed gold, utility gray, Lo-Fi dusty rose) and
    with a title row laid out as every card's (the source cards' too): its icon and its name in title case at the left,
    then a chevron that folds it, its power switch and an × to remove it at the right, in the corner (the chevron
    showing while the mouse is over the card, left of what always shows, so nothing moves as it shows); and under it a
    live display, which
    draws the effect as modulation takes its params now (the newest voice's, or the macros' while nothing plays); the
    Param EQ's band handles and the 3-band dynamics' threshold lines stay where they are set, as the filter's handle
    does. Its knobs go in rows across it, under the display: three to a row 30 px across (36 in the one row of a card
    that has one), and on a card one place wide, four to a row 26 px across and five 24, leaving room between them
    for their labels (a wider card's keep 32 px); a label stays as wide as a 32 px knob's, so a value still fits
    under a smaller knob. Two rows sit 55 px apart, the lower one at the card's foot, and a reverb's three 54 px apart,
    so the display above them takes the rest. The flanger's and frequency shifter's lower row, Feedback and Mix
    with TZF or nothing between, sits a little further in from the card's sides than a row of three's ends (21 %
    of the card), so Feedback's label stays clear of the edge:
    - the shaper's transfer curve; a Custom curve shows its own points and bends instead, to drag right there
      (double-click adds or deletes a point, Ctrl snaps to the grid), with what the drive, output and mix make of
      it faintly behind and the point under the mouse in numbers; its edit button (a pencil, at the right of its curve's
      list) opens the big editor, and is lit while it is open;
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
    - the phase disperser's delay across frequency, peaking at Freq (where Key Track moves it with the note, the
      caption reading where that is) and again at each copy Stretch makes, with what it puts out, measured, behind it
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
    another in the middle, two speakers facing apart, and their polarity's + | − at the right), over their Rate knob, which, synced, picks a note
    value and shows it as its label, as the delay's time knobs do. The chorus has its voices, 2 | 3 | 4 | 6, in the
    middle under its display, and its knobs in two rows, Rate, Depth and Delay above Feedback, Spread and Mix. The filter's slope is a list under its display, like a voice filter's (see the inspector) and a split's. The filter's
    knobs come in two rows, Cutoff, Reso and Key Track above Drive and Mix, and so do the
    frequency shifter's, Range, Shift and Spread above Feedback and Mix.

    The amp's card is 1.85 places wide, the amp on the left and the cabinet on the right with a faint line between, in
    the order the sound goes through them. The amp's side: the model's list, its display, STEREO centered under it (the phaser's two speakers), and Gain,
    Volume and Presence over Bass, Mid and Treble. The cabinet's side: its
    power symbol, then the cabinets' list, its display, the mic's three (DYN, CND, RIB) under it, and Position and
    Distance over Output and Mix. With the cabinet off, its display, the mic's switch and Position and Distance dim,
    though they still turn and take modulation.

    Each effect has an icon, at the left of its card's title and in the list of effects. On the card, the power
    switch in the title row turns the effect off (the icon turns grey with the card, under the mouse too, and the
    effect costs nothing) or on again, in its color; a click on the icon does the same, and dragged, the icon moves
    the card like the title. Folded, the strip keeps the icon, a grip, the power switch and the ×. **+ Effect** (a
    drawn plus, the word and a chevron, neutral until the mouse is on it or its list is open, then in the theme's
    accent, as + Source is) at the panel's top left opens a list of the effect types (beside it, how many of the 32
    places are taken, as in 3/32), and so does a dashed card with a plus after the last effect, at the mouse (moved
    back inside the editor where it would reach past an edge). The list groups the effects by kind in two columns,
    a little apart, each group under its name in capitals on a darker strip: DRIVE (Shaper, Amp, Lo-Fi), EQ & FILTER (DJ EQ, Param EQ, Filter) and DYNAMICS
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
    a narrow dashed card (Add to Low, Add to Mid or Add to High, as Add Effect is) that opens the list of effects at the mouse (effects only: a band holds no split), the one
    picked going at the end of that band, with no arrow before it. The arrows inside the container add an effect to
    its band there (the first, after its controls, at the band's start). The main line's arrows, the dashed card at the row's end and
    + Effect offer **2-band split** and **3-band split** as one more group, SPLIT, the one over the other under
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
    node: move · click wire: insert". Behind it, as behind the routing graph, the accent glows from OUT (one and a
    half times as strong, from the smaller box, with the same loudness, reach and Glow setting, its distances worked
    out again as OUT moves) inside its box's rounded corners, and three orbit rings go round OUT, closer together
    (108, 204 and 300 px out) and fainter on the box's darker ground; they move with OUT as the graph scrolls. The
    graph's nodes sit on a fixed grid, a column each, and
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
    and so does a join, which goes with its split. + Effect puts the new
    effect after the node picked: after an effect on its line or band, after a join on the main line, and at the end
    of a split's shortest band (the highest of those as short); with nothing picked, at the line's end. A copy goes right after its original. Wider than its box, the graph
    has a scrollbar under it (its thumb dragged, or the track clicked to jump there), and scrolls sideways under the
    wheel too or with the middle button held; it shows a node just added. Switching to the
    graph or back changes nothing but the view: both show the same line, splits and all. A click on a node shows it in the card at the right;
    right-click an effect's node for its card's menu, a split's or a join's to remove it. The node picked is saved
    with the patch; the effects' count beside + Effect and the target menu follow the line's order.
  - **Mods**: takes the place of both too: every modulation as a row of a table, by source (the LFOs and envelopes
    in the order their cards show, then the macros, the note source, the mod wheel, velocity and the pitch bend), the
    first of a
    source's rows naming it with its handle's look and the others a line in its color down the source's column.
    A row names an effect's param by the effect after FX, not by its slot's number as hosts do ("FX Chorus Rate" for
    "FX 7 Chorus Rate"), numbered along the line only where it holds more than one effect of that type ("FX Chorus 2
    Rate"), so a shaper or filter effect stands apart from a voice's ("FX Shaper 1 Drive", "Shaper 1 Drive"); so do
    undo's tooltips and the editor's messages.
    Modulations at 0 % are listed too, and after them the empty ones (see below). Over the rows, the columns' names,
    shown with no modulations too (with a hint under the button that adds one), and at the right of their row the
    count (how many are bypassed, at 0 % and empty with it, as much of that as fits), then the **filter**'s
    magnifier. A click on it opens the filter, a text field over the count that takes the keyboard: what is typed
    narrows the rows to the modulations whose source's and target's names hold every word of it, whatever the case
    (the empty modulations stay, having yet to get theirs), and it says at its right how many it shows of all.
    Its × empties it, Escape empties it and then lets the keyboard go, as Enter and a click elsewhere do; while it
    holds something it stays open. A source card's Mods button empties it, so the source's rows show. A row has,
    from its left:
    - the bypass dot, which switches the modulation off and on again, whatever its source;
    - the **source** and the **target**, and between them, after the source's name on the first of its rows, the
      source's **polarity** switch: a wave about a middle line while the source is bipolar and humps on a baseline
      while it is unipolar (the LFO's UNIPOLAR icon), flipping it between the two for all of its modulations, as its
      right-click menu does. A click on the source or the target opens a cascading menu, the next level opening beside
      an entry under the mouse. The source's lists the sources by category (LFOs, the Random and Curve sources among them, Envelopes,
      Note & MIDI for the MIDI sources, Macros, Keyboard:
      Note, Velocity, Mod Wheel and Pitch Bend, and Operators: Op A to Op F; a category of more than 16 sources, and the
      target's Modulation, go on in a second column of about as many beside the first); picking one moves the modulation there, into a free slot, with its depth,
      curve, Amount, aux and bypass, as one undo step; it then swings as its new source does. What modulates its depth
      follows it to its new slot, or, moved to a macro, the note source, the mod wheel, velocity, the pitch bend or an operator (whose depths
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
    - the **aux**: a chip in its color with its name, its depth and an × that takes it away, or a dashed **+ Aux**; a
      click on the name or + Aux opens the menu of sources, None first, the row's own source faint. The depth, a
      number before the ×, drags up and down (100 % in 100 px, finer with Shift); a double-click puts it back to
      100 %, and Ctrl+Click types one. A bar along the chip's foot, in the aux's color, fills as far as the depth;
    - the × that removes the modulation. Removing a modulation, here or anywhere else, removes whatever modulates
      its depth with it, in the same undo step.

    The right-click menu of any of a row's cells is its modulation's (Edit depth on the depth, Bypass, Remove).

    **+ Add modulation**, a dashed button after the last row, adds an **empty modulation** there and opens its
    source's menu. An empty modulation is a row with no source or no target yet ("Source…" and "Target…" in their
    places, a hollow dot, and only its ×): it modulates nothing. Picking its source and its target from their menus,
    in either order, makes it a modulation of that source, as its handle dropped on the target makes one (at +30 %),
    (an effect taken out, or a split's crossover or a join's level reset, takes the target of an empty modulation onto
    it away, as a source removed does its own),
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
    edit button, a pencil, in its corner), or the expand button in its title row, left of its power switch, widens its card to two card places, the cards after it moving along and the row scrolling to show it
    whole, to edit the shape right there while the panels above stay in use: the inspector's knobs, the routes and
    the other views can be turned and switched as the shape changes. The widened card keeps the source's own controls where a normal card has them,
    with the reset button (a circular arrow, which puts back the shape it started with) and the shape tools in a row
    where its display was, level with the plot's foot (see below), and the shape's plot beside them, 264 px across;
    under the plot, Points | Steps and Snap as their icons alone (named in their tooltips), then the grid's
    steppers, and over it, as its tooltip, how to edit it. Its title row has, before the fold, power and remove
    buttons, the collapse button (two arrows pointing in), which takes the card back to its size, and right of it the
    full-size button (two arrows pointing apart); another shape, folding the card and removing it take it back to
    its size too. Every edit applies as it is
    made, so there is no done button. One card is widened at a time: widening another takes the first one back.
    The Seq view covering the modulation panel leaves the card as it was, widened or not, for when the panel shows
    again; it isn't saved with the patch. The full-size button opens the shape in a panel as wide as the routing panel,
    in the middle of the panels' row over the routing panel and the inspector, which it hides while it is open, the card staying widened under it with its own editor greyed out (it takes no
    input there meanwhile); the done button (a check mark, top right) closes it, as a view tab does, and the card's
    editor is back in use; so does a click on the card's plot, which is dimmed while the full-size editor has its
    shape. A Curve's full-size editor has, in its title row after its name, its clock modes' list, its Unipolar and
    sync switches and its Rate (Length, or the note value synced), Phase and Slew as small knobs, as the card has them
    and working as the card's do (the list opening under its own button there). An operator's drawn envelope's
    full-size editor, the note source's curve's and velocity's are laid out the same (one panel, one title row, the
    plot, and under it the point readout or how to edit, Points | Steps, Snap and the grid).
    A Curve source's **loop** (see the modulation above) shows as a bar in its color on a strip over the shape: in
    the full-size editor over the plot, with the ruler's labels under the bar (times, beats or shares, as its places
    read, on round values), and in the widened card along the top of the plot, with the ruler's labels at the plot's
    foot. It is set as the Seq view's loop is: drag an end to size it, its middle to move it, or across the strip
    outside it to draw a new one, on the grid's columns, its ends staying a column apart; a double-click loops the
    whole curve again. With Shift or Ctrl held, a drag goes off the columns, where the mouse is (an end, the loop
    moved as far as the mouse goes, or a new loop), its ends still a 256th of the curve apart; pressed or let go
    mid-drag, from there on. Each drag is one undo step. Over the loop the mouse shows a move cursor, elsewhere a left-right
    one. Its ends are dashed across the plot, and what the clock mode doesn't play is dimmed: past the loop in
    Retrigger, either side of it in Free and Anchor, nothing in Sustain. One-Shot, which plays the whole curve, shows
    no bar. A point's right-click menu has **Set as Loop Start** and **Set as Loop End**, which put that end exactly
    at the point, on the grid or not, each one undo step (an entry that would put an end at or past the other is left
    out), before Delete point.
    The **shape tools**, a row of icons (named on hover), each change the whole shape in one undo step, a quick
    second click being another: **Invert** flips it upside down; **Reverse** plays it back to front, the first point
    staying first and each segment bent the other way; **Shift left** and **Shift right** move it a column of the
    grid earlier or later, round the cycle (where the new start falls, a point is put on the curve if there is none
    there, splitting its segment without changing its course); **Copy** and **Paste** copy the shape and paste the one
    copied, as Copy Shape and Paste Shape do (Paste faint until a shape is copied). **Copy Shape** and **Paste Shape**
    are in the right-click menu over every drawn shape (after a point's or a bend's own entries; Paste Shape only once
    one is copied), and **Ctrl+C** and **Ctrl+V** with the mouse over it do the same: a Curve source's, an operator's
    drawn envelope, a voice shaper's or a shaper effect's Custom curve, the note source's and velocity's curves, and a
    sequencer lane's curve (BEND, MOD WHEEL and the macros', over the lane, where Ctrl+C and Ctrl+V over the roll still
    copy notes). The clipboard (`src/gui/shapeclip.h`) holds the shape as it is drawn, whatever it came from: its points
    along it, their levels, their bends, whether it runs on round its end (an LFO's, a lane's) and its loop region (a
    Curve source's, an operator envelope's); a symmetric curve as it sounds, its left half its right half turned round
    the middle; a lane across its pattern, up its range, a point put at its start where its way round passes it. A
    paste is one undo step and fits the shape to where it goes: as it is into a Curve source or an operator's envelope,
    with its loop region if it has one (else the target's stays); into a shaper curve with its first and last points
    at its ends (a shape that runs round its end getting a point at the end at its first point's level), a symmetric
    curve staying symmetric with a point in its middle (put on the curve); into the note source's or velocity's curve
    thinned to its 16 points, keeping its ends (one at a time, the point whose going changes the shape least, the
    segment left across its place bent as near it as it can be), saying "Pasted 16 of its 41 points"; into a lane
    across the pattern's length in steps and up the lane's range. Each says where it went ("Shape pasted into Op A
    Envelope"), Ctrl+V with nothing copied "Copy a shape first". **Points | Steps** picks what a
    click on the shape does, in the card (under its plot) and in every big editor below, which has them too (left of its GRID, or in
    a shaper effect's narrower editor in its title row, left of the reset button): with Points, as below; with
    Steps, a click draws a step, as Alt+click does with Points, and Alt+click works with points. **Snap** snaps
    points and steps to the grid without Ctrl, which then lets them go (a shaper's curve drawn right in its card or on
    its page, which show no grid, snaps with Ctrl only). Both belong to the editor, like the grid, and go for every
    shape and curve. How to edit a shape, as they have it, shows under each full-size editor's plot, where the point
    under the mouse is shown in numbers, on two lines in smaller type where one doesn't fit, and as a widened card's
    plot's tooltip.
    Drag points to move them; the point under the mouse is shown in numbers, its level as the LFO puts it out (0 to 1 for a
    unipolar one). A point stays between its neighbors, but can reach a neighbor's time: points at one time make a
    line straight up or down (a spike, say), and at that moment the LFO takes the last of them in order, the one
    the shape goes on from. Double-click empty space to add a point and a point to delete it (or right-click it).
    **Picking several points**, in every shape editor (a Curve source's, in its widened card and full size; a shaper
    effect's and a voice shaper's drawn curve, on the card or page and in the big editor; the note source's and
    velocity's curves): a drag from empty space draws a box that picks the points it reaches (with Steps, Alt+drag,
    since a plain click draws a step there), and with Shift adds them to those picked already; Shift+click on a point
    picks it along with the others, or lets it go if it was picked. A click on a point not picked picks it alone, and
    on one of several picked, let go without moving, picks it alone too. Picked points are filled and ringed. Dragging
    any of them moves them all as far as it moves, in one undo step, keeping their spacing: across, as far as the
    nearest point not picked either side lets each go (and the first point, a curve's last and a symmetric curve's
    middle stay where they are across, moving only up and down), and up and down as far as the shape's top and bottom
    let every one go. Ctrl+A picks every point of the shape under the mouse (else the big editor's, else a widened
    card's), and Delete removes those picked, all but the first, a curve's ends, a symmetric curve's middle and as many
    as keep two points, as one undo step. A click anywhere off the shape, any change of how many points it has, and
    the shape tools and reset let the points go. What is picked isn't saved.
    Alt+click (over a point or a bend dot too) draws a step, as in a step sequencer: a level line at the
    click's height from the grid line left of it to the one right of it, with a point at each end (Ctrl snaps its
    height to the grid too). The points between those lines go; a step beside another starts where that one ends,
    at the same place, so the shape jumps straight up or down there, and a step in the first column takes the
    first point. Held, the step follows the mouse: up and down to another height, and across to another column,
    the one it left going back to how it was, until the button is let go (one undo step). Alt+click a step
    again to redraw it at another height. An LFO's shape and a shaper's curve
    hold 64 points, so every column can hold a step even on the finest grid, 32 across; the note source's curve
    and velocity's hold 16. The small dots halfway along each segment bend it (a jump has none): drag them up or down,
    double-click to straighten. **GRID**, under the
    editor, sets how many lines run across (2–32, thirds included) and up and down (2–16), 8 and 8 to begin with, with its − and +
    buttons or the mouse wheel; hold Ctrl to snap points to them. The grid belongs to the editor: it is saved with
    the project, and presets leave it alone (so do Points | Steps and Snap). The first point always sits at the start of the cycle. A shaper
    effect's Custom curve opens the same editor from the edit button (a pencil) beside its card's curve list, in a panel in the middle of the
    effects' (the inspector stays hidden), its plot closer to square, with input across and output up; its first
    and last points stay at the ends. While the shaper is symmetric, only the curve's right half is edited, in
    the editor and in the card alike: the left half is drawn as its mirror (behind a veil in the editor) with faint
    rings for the points it mirrors, the points left of the middle are hidden, and a click there does nothing (no
    point added, no step drawn; a step held across the middle stays where it last was on the right). The editor has
    its own **SYM** switch, left of Points | Steps (in the title row for a shaper effect, under the plot for a voice
    shaper), as the card and the shaper's page do. Switching Symmetric on gives the curve a point in the middle if it has none,
    which then stays there: it only moves up and down, and can't be deleted (a step drawn across it keeps it, at the step's height). The note source's curve opens in the full-size editors' panel from its display in the
    keyboard strip (a second click closes it): the notes from Low to High across, the Cs among them named under
    the plot, and what the source puts out up, from nothing at the bottom to all of it at the top, with a dot
    where the newest voice is while notes play. A dashed line shows, for the range set, what would take a cutoff
    at 0 by 100 % onto the note. **Low** and **High** are knobs side by side in its title row, dragged in whole
    notes (or typed, as `C4`, `F#2 -20` or a MIDI note number); its ends stay at the ends too, and the reset button
    brings back every MIDI note and the curve along the dashed line. Velocity's curve opens in the same panel the
    same way, from its display (see Velocity, under the keyboard strip).
- **Header**: two rows. The **title bar** holds the name at its left, the preset bar in its middle, and undo and
  redo, Init, Save and the overflow menu's **...** at its right (see Presets and Settings, below). Left of the
  tempo, while an MTS-ESP master or the patch's own tuning tunes the synth (see Microtuning and Tuning files), a
  tuning fork as large as the H, in the accent for a master and the text's color for the patch's tuning, its tooltip
  saying which and naming the scale ("Tuned by MTS-ESP: 19-EDO" or "Tuning from the patch: 19-EDO"). It is a mark
  alone: a click on it does nothing. With nothing tuning the synth, none; nor for MTS SysEx, nor for a master while
  MTS-ESP is off. Between the name and the preset bar sits the **tempo** every synced LFO, delay and sweep follows: a switch, **H** ("Sync to
  host tempo"), then a field with the tempo in BPM (20–999). With the H on, as it starts, the synth plays at the
  host's tempo, which the field shows dimmed (as the synth last played it) and which takes no edits there. With the
  H off, the synth plays at a
  tempo of its own (140 to begin with), which the field shows and sets: dragged up or down it moves a BPM every 2 px,
  onto whole ones (a tenth of one with Shift); the wheel steps it a BPM a notch (a tenth with Shift); a double-click
  or Ctrl+Click types one, with or without `BPM` after it, held to the range. The host's transport keeps playing it
  where it plays: anchored LFOs and the sequencer in Host count its time in beats of the synth's own tempo. The H
  and the tempo of its own are settings of the instance, as the oversampling and MPE are: a host's project keeps
  them (one saved before them following the host), presets don't, and undo leaves them alone. A preset saved keeps
  the tempo it played at (its own, else the host's, a DAW's too), and loading one saved at a tempo sets the synth's
  own tempo to it, so with the H off a sequencer pattern plays as it was made. Init and a preset saved before this
  leave them as they are, and a preset loaded in a DAW leaves the host's tempo alone. A host that stands in for a
  transport and lets the editor set its tempo, the browser demo and `fmvibe-uihost`, has no H: the synth always plays
  its own tempo there, which the field sets from the start and the host's transport follows (`fmvibe-uihost`'s
  starting at its `--tempo`, the demo's kept in the browser's storage, its first preset setting it as the page
  opens). At the tempo's right, while the sequencer is on, the SEQ card's **Play** button shows again as its icon
  alone (`kTitleSeqPreview`, a `SeqPreview` widget the SEQ view presses and paints), in whatever view shows: a
  triangle, and a square lit while the pattern plays (from either button). It plays from the first note picked in the
  roll only while the roll shows, and from the loop's start otherwise (`SeqPanel::seqPreviewStart()`). With the
  sequencer off it isn't there. Under it, the
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
  and Porcelain, as their cards, on its shadow alone, without an outline; a filter's response and a shaper's curve on
  it a shade darker than the displays elsewhere in Porcelain, whose displays are nearly the lens's own face); while
  the graph shows that node, a notch on the card's right side meets the leader from it. Tabs A–F, F1 and F2, and S1
  and S2, in three groups with a hairline between two, on a band a shade darker across the card's top with a
  hairline under it parting them from the page, pick what it shows; tabs of switched-off nodes are
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
    that keeps Freq in its range), in the same undo step, so an A4 sounds as before; Freq (a knob) and Mult (a stepper) take the
    Ratio readout's place, Pitch and Detune are dimmed and take no drags, wheel or typing (nor do their depth
    handles), and the summary line and the operator's circle in the graph show the frequency in Hz instead of the
    ratio. Switched off, the ratio, Pitch and Detune count again as they were. While the wave is Custom,
    an edit button (a pencil, the same as a Curve source's and a drawn shaper curve's), left of the keyboard, opens
    the operator's partial editor (lit while it is open; a second click closes it, as does picking another operator, filter or shaper in the
    inspector). Under the block, past a line across the page, a well: tabs along its top edge pick what the
    display under them shows, and the tab's controls sit in a row under the well (I/O, which has none, takes the
    well down to the lens's bottom). The tab is the same for every operator and saved with the project (a preset
    leaves it as it is); a tab whose knobs a source modulates, shown or hidden, has a ring in its corner (not the
    filled dot that says a tab's contents are at work), and holding a dragged
    source's handle over a tab for half a second opens it, as over a view's tab (passed over, a moment on each, it
    opens none), so its knobs can take the drop:
    - **Envelope** (to begin with): the operator's envelope, and its knobs. While notes play, a dashed line across
      it shows the newest voice's envelope level, with a dot where it meets the curve, as a mod envelope's card
      does. The curve has the whole display; its controls are in the row under the well, which starts with **ADSR**
      over **Curve**, one pill of two icons (an AHDSR's shape, and a curve through its points, named in their
      tooltips), then the AHDSR's five knobs, a little closer together for it; with Curve, One-Shot | Sustain (the
      Curve source's icons), its sync switch (the metronome), Length, Slew and, at the right, a pencil. Switched to Curve
      the first time (its shape still
      as a new patch has it), the curve is drawn from the AHDSR in the same undo step: the attack, hold (none while Off) and decay, a
      flat stretch at the sustain level as Sustain's loop (a quarter of the other times, at least 50 ms), then the
      release, the segments' curves bent alike, in their proportions, Length 1 s. The display then shows the drawn curve, edited in
      place as a voice shaper's curve is on its page (points, bends, Points | Steps and Snap from the grid's
      settings), with its loop's bar along its top in Sustain and the ruler's times under it, clear of the curve's
      foot; Length is a note value while synced. While notes play, a dashed line across shows the newest
      voice's level and a dot where it is on the curve. The pencil opens its full-size editor, laid out as a Curve
      source's: in the same panel, over the routing panel and the inspector, with its name, One-Shot | Sustain as a list (as a Curve's
      clock modes, with their icons, opening under its button; the wheel and its arrows step it), sync, Length and
      Slew in its title row where a Curve's has its clock list, sync, Rate and Slew (it has no Unipolar switch and no
      Phase), the loop's strip and ruler over the plot, Points | Steps, Snap and the grid under it, the point under
      the mouse in numbers, and the point menu's Set as Loop Start and Set as Loop End; Done or a view's tab closes
      it.
    - **Unison**: a display of where the copies sit (left/right by spread, up/down by detune, each drawn as large
      and as strong as Blend leaves it; a Random mode as one note might spread them), with a row of buttons along
      its bottom, each an icon named in its tooltip: the four Unison Curves (five copies' pitches as ticks), then,
      past a rule, the three Unison Pans (five copies as dots, rising in pitch, placed across). Under it the knobs:
      Unison (the copies), Detune, Spread, Blend and Rnd Phase.
    - **Warp**: the operator's scope, its warp and its note sync. Its display is a scope: the operator's output in
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
      the warps as buttons, each a sine as it warps it (named in its tooltip), the Amount knob and the NOTE switch, an eighth note,
      (Note Sync, named in its tooltip). The audio thread captures only while the tab is shown. The summary line names the warp and says "note
      sync" while either is on. (The scope and the warp had a tab each before; a layout saved on the scope's opens
      this one.)
    - **I/O** (the list a filter's and a shaper's page show under their displays too): under an INPUTS heading, a
      row per node routed into the operator (the nodes that are on, in the
      graph's order): its name, which shows it in the inspector as a tab does; four buttons with the modes' glyphs,
      the one in force lit, each naming its mode on hover; the mode's name; and the route's depth as a label,
      dragged, typed and modulated as a wire's. Its feedback comes last, with its depth and "phase only". Under an
      OUTPUTS heading, a row per node the operator is routed into, likewise: an operator's with the route's mode
      buttons and name, a filter's or a shaper's with its title in their place (a route into one takes no mode);
      then its line to OUT, with its output's depth as the graph's OUT label. A section with no route has a line
      saying so ("No inputs", "No outputs"), fainter than the headings. The rows sit 30 px apart,
      room for a modulated depth's dots under it; past what the tab holds, they scroll, by the wheel over them, by
      dragging with the middle button held, or by the scrollbar at their right.

    A noise operator shows Slope and Spread in the Ratio readout's place, and no Pitch, Detune or Phase; its first tab is
    **Spectrum**, its spectrum against white, pink and brown, and it has no Warp tab (picked, it shows its
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
    curve's edit button (a pencil) and the symmetric switch between them (as on the shaper effect's card), the Drive,
    Output and Mix knobs, centered under them
    (what goes straight out is its line to OUT's label),
    and its transfer curve; a Custom curve shows its points and bends there to drag, like a shaper effect's card, and its
    edit button opens the big curve editor over the routing panel, the shaper's page staying beside it, the button lit
    while it is open. Underneath, in a well of its own, its I/O list, as an operator's I/O tab has it (see below):
    under INPUTS the routes into it, each its node's name and title and its depth, its own route into itself last
    ("S1 feedback"); under OUTPUTS the routes out of it, into an operator with that route's mode buttons and name, and
    its line to OUT; a section with none a line saying so ("No inputs"). Past what the well
    holds, about three rows, the list scrolls as an operator's does.
  - Filters: the summary (its type and slope, then what feeds it, what it feeds and whether it goes out, and the
    operators it modulates, as a shaper's has), the type's list across the row under it (where a shaper's curve's
    is), offering the seven families (Low-pass, High-pass, Band-pass, Notch, Comb, Formant and Morph), its button and
    each of its entries a small picture of the family's response before its name, the wheel over it and its arrows
    stepping along them (a family picked keeping the model where it has it, Ladder's high-pass for a ladder, say, else
    taking its first), then in the same row, for a low-, high- or band-pass, the **Model** list (SVF, Ladder, Diode
    and Sallen-Key for a low-pass; SVF, Ladder and Sallen-Key for a high-pass; SVF and Ladder for a band-pass), short
    as on the filter effect's card (its button the model's short name, without arrows or icon; its list each model's
    small picture before its name), the wheel stepping it held at the ends, or a morph's notch switch (the notch's
    icon) in its place, and the slope's list at the row's right end, each where it is whatever the type (so the type's
    arrows stay put as it steps); then cutoff, resonance (a formant's Shift and Q; a formant's Vowel or a morph's Morph
    after them, the six knobs a little closer together), key tracking, drive and mix (its output level is its line to
    OUT's
    label), and a live response curve, drawn as its Mix blends what goes in back in (a notch at 50 % dips about 6 dB,
    a low-pass leveling off 6 dB down past its cutoff), whose
    handle drags cutoff (sideways) and resonance (up/down), centered under the knobs, as tall as a shaper's curve and
    as wide. The slope's list offers 6, 12, 18 or 24 dB, as the filter effect's and a split's do (a band-pass's or notch's shows 6 and 18 faintly and can't pick them, and shows
    what plays; for a comb it is faint and doesn't open). With its cutoff or resonance modulated, faint curves
    show how far the modulation takes it either way, and while notes play the curve follows where the newest
    voice is (the handle stays at the set values, the cutoff a C4 has). Key tracking moves all of it with that
    voice's note, an octave per octave at 100 %. The filter effect's display does the same, its Mix drawn in too, with a handle of its own for cutoff and resonance, its key tracking
    staying where the last note left it, as the effect's does. It sits on the filter effect's grid. Behind the curve,
    like the filter effect's, is what the filter puts out, all voices together, measured before its output level
    (the Param EQ's analyzer: a 2048-point FFT in 96 bands, 0 dB at the top), while the editor is open; a filter
    that is off shows none. Underneath, in a well of its own, its I/O list, as a shaper's (and an operator's I/O tab)
    has it.
- **Modulation**: the panel's title row has its title, **MODULATION** (the panel doesn't collapse; the Seq view
  covers it), then **+ Source**,
  shaped like + Effect (a drawn plus, the word and a chevron), which opens the list of source types under it to add
  one, and beside it how many of the 32 places are taken, as in 4/32 (with every
  place taken it is dimmed, and a click says so). Random and Curve cards are laid out as an LFO's (see below), with
  their clock modes' list where an LFO's shape list is, their Unipolar and sync switches under it and three knobs
  beside it. A Random card's Unipolar switch sets which way its depths swing, as the polarity in its title's menu
  does (it starts bipolar, the switch off); it has no Phase knob. A MIDI card has its input's list (CC,
  Pressure or Slide, where an LFO's shape is; its tooltip says what the input follows), and right of it its CC stepper
  (while its input is a controller) and Slew; under them, both cards'
  display is a bar of what the newest voice gets now (from the middle while the source is bipolar), with its value.
  Below, the sources' cards show four at a time, and after the last one, while there is room for more, a dashed
  card with a plus, **Add Source**, as tall as the cards, opens the list of source types at the mouse, moved back
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
    envelope follows its curve, the dot sits where the line meets it. Where it doesn't (a note let go during the
    decay releases from above the sustain), the dot keeps to the curve and the line shows the level. An envelope's
    display (on a mod envelope's card and an operator's Envelope tab alike) draws its curve as modulation of its
    attack, hold, decay, sustain, release and bends takes it for the newest voice, or with no note playing as the
    macros take it, a faint line staying where the knobs set it, with the handles on it. Its time axis leaves room for
    the times at their longest under that modulation, so it stands still as the curve moves.
  - A card's title row is laid out as an effect card's: the handle and the name in title case ("LFO 2", "Env 1") at
    the left, then the fold chevron, a Curve card's buttons, the power switch and the × at the right.
  - The power switch turns a source on and off; one that is off has its card dimmed, its name grey (as an effect's is), and its handle and its rings
    on knobs faint (the handle can still be dragged onto targets). Like a macro's, a source's handle is faint too
    while it modulates nothing (and scales no modulation as its aux).
  - The chevron left of the power switch folds the card to a narrow strip, as on the effects and as wide as theirs
    (the handle as far in from the card's edge as an effect's icon, so it stays put): the handle, a grip
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
  - A card doesn't list its modulations, and ends with a link under its display, words without a button's face:
    it counts them in dim text ("1 modulation", "3 modulations"), the words themselves the link, lit under the mouse,
    and shows them in the Mods view, scrolled to the source's first row. While the source
    modulates nothing, it reads **No modulations** alone and adds an empty modulation from the source there (see the
    Mods view), with its target's menu open.
    A Random
    card has **Smooth** and **Chaos** knobs beside its Rate, and its display shows four made-up cycles of what they do;
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
    along them). Under the shape list, as wide as it, is its clock modes' list: its
    button shows the mode it runs in, its icon and name, with arrows that step along the list, round its ends, and the
    wheel over it steps along it too; its list shows the modes with their icons, the one in force checked (the button
    shows no tooltip, as the shape list doesn't). Its icons: a rise that starts again at each dot for **Retrigger**,
    an infinity sign for **Free**, an anchor for **Anchor** and a single cycle that ends in a dot for **One-Shot**. An
    LFO has those four, a Random source the first two and a Curve card five, with **Sustain** last (a rise, a level
    between two ticks and a fall); a Random's and a Curve's list is where an LFO's shape list is. Each pick, step or
    wheel notch is one undo step. Right of the two rows, centered on them, are an LFO's Rate and Phase knobs.
    Between an LFO's shape list and Rate, level with the list, is the
    **Unipolar** switch, a hump on a baseline as a Mods row's polarity switch has, and under it, level with the clock
    list, the sync switch: a small square button with the metronome the effects' SYNC has, which turns the Rate knob
    into a note-value knob (as it starts, on). A Curve card and a Random card have their Unipolar and sync switches
    under their clock list instead, at its right end before the Rate knob, and right of the list three knobs alike,
    side by side: a Curve's Rate (labeled Length while it reads as one), Phase and **Slew**, a Random's Rate,
    **Smooth** and **Chaos**.
    The Phase knob is faint while it does nothing (on a free LFO that isn't anchored), and otherwise
    a thin line on the display marks where notes start the LFO (or where the song starts an anchored one). A widened
    card's plot and the full-size editor show it as a faint dashed line (none at 0 or 100 %), drawn solid while the
    Phase knob is held. A Curve's
    dot keeps to its curve, as an envelope's does, and a dashed line across, while it runs, shows the level it puts
    out, which a release in Sustain takes off the curve until the next point (in its widened card and full-size
    editor alike). A
    unipolar LFO's display has its zero line at the bottom. A Curve source's
    display, or the expand button in its title row, widens its card, to edit the shape there (see the shape editor);
    a new one starts widened.
  - A new modulation starts at +30 %, upward, whatever the target's setting. The target then shows a colored ring
    for the modulation range, plus a small dot of the same color under the
    knob (on a matrix cell, centered along its foot, the ranges along its top, right of a mode's glyph; on a wire or OUT label in the
    graph and on the I/O tab's labels, centered just under it, the ranges running along its outline's top, so
    its depth stays readable; the graph's labels, 19 px tall with their depth in the size of list rows,
    have dots as large as a knob's, 10 px apart, and their ranges a little thicker). Where a range runs past its target's end, cut off there, a short line across the
    ring or bar marks that end (in every theme; the Ratio readout's track has its arrow instead). The dots stay
    inside their knob's column, label or cell at each handle size; past
    what fits, the rest go into a "+n" pill. Drag that dot to set the depth; double-click it
    to remove the modulation, or right-click it for a menu that types its depth, bypasses it, types its aux depth and removes its aux
    (when it has one) or removes it. While the dot is under the mouse or dragged, its range (the ring, or
    the bar on a label, cell or depth bar) is drawn wider, over the other modulations' ranges where they overlap,
    and while its source's handle is on screen, a dashed line runs from the handle to the dot, as when the handle
    is hovered; with an aux, a dotted line runs from the aux's handle too. A source can modulate another source's depth too (drop its handle on
    a depth bar in the Mods view): the bar then has the dot in its top right corner, a strip under it shows
    how far the depth swings, and hovering the handle draws a dashed line to it like to any other target.
  - **More than fit**: a knob, label, cell or depth bar shows as many depth handles as its width holds (under a 24 or
    26 px knob 2, under a 30 or 32 px one 3, under a 34 px one 4; on a wire label in the graph 4, on the I/O tab
    6, a matrix cell 5, a depth bar 11), in the order the
    sources show. Past that, as many as fit beside a **+n** pill (2 under a knob, 3 on a label, 4 on a cell, 9 on
    a depth bar; the graph's labels' as large as a knob's) hold the rest, so nothing runs under the next control. Hovering the pill draws the ranges of the
    modulations it holds wider, and says how many there are; a hovered source whose depth handle the pill holds
    draws its dashed line to the pill, ringed in the source's color. A click opens the pill's list, under it (over
    it, short of room below), in the accent color while it is open: the target's name and every modulation of it,
    a row each: its bypass dot, the source's name, its aux (while one of the rows has one, the list is wider by a
    column for them: a chip in the aux's color with its name, its depth, as in the Mods view, and an × that takes it
    away), its depth bar and its
    remove (right-clicking a row gives the depth handle's menu; hovering a row draws its range
    wider; a source's name too long for its row is drawn smaller to fit, down to about 9 px, and past that ends in
    "…"). It shows six rows at most; past that, it is 8 px wider for a scrollbar at its right, and the rows scroll by
    the wheel over them (off the depth bars, which the wheel turns), by dragging with the middle button held, or by
    the bar. A click anywhere else closes it, but not
    on a source's handle, so a handle can be dragged onto one of its rows. A source's handle held over a view's tab
    (Routing, Matrix, FX, Seq or Mods) for half a second shows that view, so it can be carried to what is there; held
    as long over a node's tab atop the inspector, a node in the routing graph (or its edge), or a node's name in the
    matrix (a row's or a column's), it shows that node in the inspector, as it is, switching nothing on. Passed over them, a moment on
    each, it shows nothing: the half second starts again on each tab or node, and the poll shows it while the handle
    stays still. Held over a pill, it opens
    its list too; let go of on a row, the source becomes that modulation's aux (see below), and anywhere else the
    list closes again. The list closes with its pill, when the modulations come down to what fits. While every
    modulation the pill holds is bypassed or its source switched off, the pill is hollow.
  - **Aux**: a source's handle dropped on another source's depth handle, on a Mods row's aux cell, or on a row of a pill's list (while it
    is dragged, the depth handles and rows are ringed faintly, the one under it brightly) makes the dropped source
    that modulation's aux, in place of any it had; the depth handle then has a ring in the aux's color, as much of
    it as the aux's depth, clockwise from the top, and the rest of it faint. Alt-dragging the depth handle up or down sets the aux depth
    (100 % in 100 px, finer with Shift), and its menu's Edit aux depth types one. A
    modulation's ring on its knob, and its range on a label or cell, show how far its depth and Amount take it,
    which an aux lets it go at most.
  - **Bypass**: the dot at the start of a modulation's row in the Mods view or a pill's list switches that
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
  - Each depth bar (in the Mods view and a pill's list) has a white tick where its modulation takes its target now,
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
- **Steppers**: the header's Voices, an operator's Unison and Mult (fixed), a shaper's Stack (a voice's and an
  effect's, without a label), a MIDI source's CC and the Seq card's Speed, Length, Octave and Transpose show as steppers
  rather than knobs: the number in a field, − and + at the field's ends and its steps along the field's foot, a tick
  each where they fit 2.5 px apart or more (the one set lit), else a track filled to the value (from its middle where
  the range has 0 there), with no line across it. Every other
  stepped value stays a knob. A stepper takes what a knob takes: a drag up and down a step every 10 px (Voices
  doubling every 40), the wheel a step a notch, a double-click to reset, Ctrl+Click or a double-click on its label to
  type, a source's handle dropped on it where its value takes modulation, its depth handles under it; − and + step it
  as a notch of the wheel does, an undo step each, a quick second click stepping again, and an end that is reached
  faint. They show faint at rest where the number leaves them room, and under the mouse wherever it clears their
  glyphs, the number then taking a smaller size if it must; a number too long even so (Mult's ×0.001) leaves only the
  one under the mouse showing, moving off it. The cursor is a hand over − and +, up and down over the number (and
  while it is dragged), and the arrow elsewhere on it. Its field is under the knob's place, as wide as the widgets
  beside it leave room for (36 to 56 px, the end of a row half the way to the next knob), and in the header under
  its label. A modulation's reach shows along its steps in its source's color (the ticks within it taking that color,
  the one set keeping its own), and a white dot where the newest voice is.
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
  panel that scrolls (the effects chain, the source cards, the Mods view's rows, an operator's I/O rows, the
  preset browser's list, a pill's open list) to drag what it shows along with the mouse, whatever is under it; past
  either end the row stops, and dragged back it follows at once. A middle press on another open list or menu does
  nothing, and one that starts a drag closes it;
  while the left button drags something, the middle one does nothing. Two fingers on a touchscreen pan the same
  panels as the middle button, along with them: what the first finger pressed ends as they land, nothing dropped,
  and over a dialog they pan nothing. In the browser demo two fingers are the page's instead: they scroll and zoom
  the page as the browser does, so on a phone the page can always be scrolled past the editor, and what the first
  finger pressed ends as the second lands, nothing dropped, the editor taking no more of either until both are
  lifted. A precision touchpad's two-finger
  scrolling comes as the wheel, up and down as above and sideways as a tilted wheel does, which scrolls what scrolls
  sideways under the mouse (the effects' row and graph, the source cards) and turns nothing. In the browser demo on
  a phone (or any touchscreen), one finger works the editor too: where nothing scrolls it is the mouse at once;
  over a panel that scrolls it waits until it moves or lifts, so that dragged on something that drags (a knob, a
  depth bar, a card's title or icon, a handle, a node) it drags it, dragged anywhere else it pans the panel, and
  lifted without moving it is a click where it landed (two quick taps a double-click, so a double tap sets a knob
  back). A long press opens the menu a right-click would. Every dialog (the
  confirm dialogs and the settings) takes the keyboard while it is open: Enter presses its button lit in the accent
  (Load, Init, Overwrite, Clear FX, Keep), Escape cancels it as a click around it does, and both close the settings;
  the dialog asking where a middle band's effects go lights none, and Enter leaves it open. An open list or menu (a
  list button's, the list of effects or of source types, the wave menu, a pill's list, a Mods row's cascading menu, the
  Seq view's lists) closes at Escape, picking nothing, as at a click around it. A double-click whose first click closed the
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
  band's shapes, the Mods view's cascading menus) marks the row under the mouse, and the one picked, in the same
  neutral tones; only a row's icon takes a color (the effect's, the band's, the source's). The type lists show an icon
  before each entry's name, on their button and in the list: an LFO's shape, a clock mode, a filter's type, a route's
  mode, a shaper's curve (its transfer curve, a drawn curve's points, Stack three layers stacked), the reverb's algorithm
  (a hall's columns, a room as a box, a plate on its wires, a shimmer's sparkles), the amp's model (Clean a gem, Blues
  two notes, Crunch a crackle, Lead a flame, Heavy an anvil, Bass a bass clef, Fuzz a spiky ball) and a MIDI
  source's input (CC a knob, Pressure a key pressed down, Slide an arrow up and down); the amp's cabinets show their
  names alone, which say their speakers.
- **Stepping lists**: a voice filter's and the filter effect's type, a voice shaper's and the shaper effect's curve, the
  amp's model and cabinet, the reverb's algorithm and an LFO's shape (its icon and name between the arrows) have an arrow at each end of their list's button, in place of the chevron: ‹ steps to
  the entry before the one shown and › to the one after, in the list's order, round its ends (from the last on to the
  first), skipping an entry the list can't pick, as fast as they are clicked (a quick second click is a step, not a
  double-click); each click is an undo step, as a pick from the list is, and closes the
  list if it is open. The arrow under the mouse lights in the list's color and says what it steps to (Previous type,
  Next curve, ...). A click on the rest of the button opens the list. An LFO holding the retired Custom shape, which a
  state can, steps onto the first or last shape offered, its widened card going back to its size as a pick does.
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
  (a chevron and the title, as far from the strip's top as the modulation panel's title from its) collapses the strip to its top
  row, taking the wheels, the keys and the octave shift away; the wheels' handles stay, their names (Bend, Mod) then
  under them, so the wheels can still be assigned. Whether it is
  collapsed is kept with the editor, and loading a
  preset leaves it as it is. It is also kept in the user's settings (`keysCollapsed`), written as it is collapsed or
  opened: a new instance opens with the strip as it was left last, while a project keeps its own (in the browser
  demo, the browser's storage keeps it). A key plays as hard as where it is clicked: velocity 1 at its top, 127 at its bottom
  (a black key along its own height). Drag across the keys to glide between notes, each key struck as hard as where
  the mouse enters it. Keys light up while they are held, whether clicked here or
  played by the host, rising and fading a moment as the roll's keys do rather than switching. The **Bend** wheel is the Pitch Bend param: it bends the pitch by the Bend Range and springs
  back to center on release, in one edit the host follows. It shows where the bend stands: the param, with what the
  host adds on top (its MIDI pitch bend, and its modulation of the param, the newest note's own while notes play) and
  the sequencer's bend curve as it plays, filled in the pitch bend source's color, and its handle drags the pitch bend
  source onto what it should move. The **Mod** wheel is the Mod Wheel param, showing what the sequencer's mod wheel
  curve adds as it plays, and its handle drags onto what it should move (not the wheel itself). Both
  handles work like a macro's (faint, like theirs, until they move something). The mod wheel's and either handle's
  right-click menus make that wheel unipolar or bipolar; a bipolar mod wheel fills from its middle, where it moves
  nothing.
- **Macros** (in the keyboard strip, above the keys): eight knobs, each in a color of its own, with a handle
  beside it to drag onto anything a source can modulate, just like an LFO's, and its name under it: double-click
  the name to change it (Enter
  keeps it, Escape doesn't, an empty name brings back "Macro 1" and so on; an undo step, saved with the patch, up to
  24 bytes of it in UTF-8, cut between whole characters).
  Hovering a handle draws lines to what it moves; the dots and rings on its targets work like a source's. A macro's
  knob takes modulation like any other knob (a source's handle dropped on it, its depth handles under it), but not
  its own macro's handle nor one that would loop, which says why as it is let go of; its dot then shows where the
  newest voice's modulation takes it, or with no note playing where the other macros and the mod wheel take it. Until a
  macro modulates something or scales a modulation as its aux, its handle, its knob's arc and its name are faint
  (less so than a switched-off control, so they can still be found and read in every theme), and hovering the handle shows a "Drag to assign" tooltip (so do the note source's, the wheels' and
  velocity's). The right-click menu of its knob,
  handle or name makes it **Unipolar** or **Bipolar** (and the handle's or name's clears its targets); a bipolar
  macro's arc starts from the knob's middle, where it moves nothing, and a double-click on its
  knob (or Reset in the knob's menu) sets it back there, to 50 %, where a unipolar one's goes back to 0.
- **Note** (in the keyboard strip, right of the macros): the note source's handle, level with the macros', to drag
  onto a target like a macro's (and like a macro's, faint until it modulates something), and beside it the note
  the newest voice plays, as the source follows it, its name under it: the note's name, and
  the cents it is off while bent or gliding (a dash while nothing plays), over a picture of its curve, faint like
  the handle until the source modulates something and lit once it does. A
  click on it opens the curve's editor (see the shape editor). Its right-click menu, and its handle's, make it
  unipolar or bipolar (± in the display's corner while it is bipolar).
- **Velocity** (in the keyboard strip, at the right end, after the note source): the velocity source's handle, to
  drag onto a target like the note source's (faint until it modulates something), and beside it the velocity the
  newest voice came with, 0 to 127 as MIDI counts it (a dash while nothing plays), over a picture of its curve
  with a mark where that velocity falls on it (faint like the handle until velocity modulates something, lit once
  it does), its name under it. A click on it opens the curve's editor (a
  second click closes it): velocity across, marked at 32, 64 and 96, and what the source puts out up, from nothing
  at the bottom to all of it at the top, with the straight line it starts as dashed behind and, while notes play, a
  dot where the newest voice is; its points and bends edit like the note source's curve, and its reset button
  brings back the straight line. Its right-click menu, and its handle's, make it unipolar or bipolar, as the note
  source's do.
- **Presets**: the library holds the factory presets, built into the plugin, and the `.fmv` files in
  `Documents\FMVibe\presets` and its subfolders. The factory presets show under **Factory**, as a subfolder's would;
  nothing of them is written into the folder, which FMVibe makes only when a file dialog needs it
  (empty: it holds the presets saved there). Each factory preset is in one or two categories (see Preset info, below):
  Bass, Drum, FX, Keys (bells and organs too), Lead (brass too), Pad (moving textures too), Pluck and Seq
  (rhythmic, gated). Up to 0.11 each name began with its category's two letters ("LD Screamer"); a project saved then
  still names the preset that way, and it loads the preset as it is named now. They are Electric Piano, FM Bass, Glass Bell, Warm Pad, Drawbar
  Organ, Pluck Delay; Crystal Motion, which uses everything at once (all six operators in three layers, two
  filters, four LFOs and a mod envelope, and six effects); Reese Bass, a heavy drum and bass reese (two stacks
  of detuned saws through a driven low-pass, a clean sub, clipping and three-band compression); Wobble Bass (an
  eighth-note LFO throwing open a resonant filter and the FM together); Shimmer Pad (unison layers through two
  filters, three LFOs and the shimmer reverb); Trance Gate (supersaws chopped in sixteenths by a square LFO, a
  filter sweep over two bars); FM Brass (two stacks that blat in, an envelope on the FM,
  vibrato); Industrial Hit (a body dropping two octaves, band-passed noise, metallic partials and a sine-fold
  shaper); Folded Lead (a mono lead whose modulator goes through a sine-fold shaper on its way to the carrier,
  driven harder as each note starts); Fuzz Bass (an FM pair through an asymmetric shaper into a plucked
  low-pass, the shaper also growling back onto the modulator, over a clean sub); Comb String (a noise burst
  ringing a comb tuned to the note, on through a 6 dB low-pass; the mod wheel damps it); Chip Pluck (alternating,
  half and abs sines like the classic FM chips, the note source easing the FM off up the keyboard, a hard clipper
  going out beside it, and a Bite macro on FM Amount, playing a video-game arpeggio, mostly in eighths, while a key is
  held, bouncing through C minor's chord and then A-flat major's, its accents as velocities louder and brighter); Step Filter (an organ wave and a pulse through a resonant
  18 dB low-pass that a drawn LFO shape opens on every eighth note, each its own height, into a cubic shaper; its
  Cutoff and Accent macros, the latter deepening the pattern); Glass Steps (a custom wave from 64 partials, the
  Fibonacci ones, playing a sparse, leaping Lydian arpeggio while a key is held, two degrees lower in its second
  bar, its accents louder, through a notch that jumps at random every sixteenth note); Vowel Lead (a vowel wave with a
  held bite and breath noise modulating it through a band-pass, gliding into every note, a bipolar envelope scooping
  into pitch and the mod wheel bringing in vibrato, a frequency shifter detuning the sides); Laser Zap (a
  one-shot LFO throwing the pitch down four octaves in a quarter second, a phase disperser and a staircase-curve
  shaper crushing it; its Speed and Grit macros); Noise Riser (wide noise through a high-pass and a climbing
  sine, swept evenly over eight seconds by an envelope, turning a frequency shifter's spiral faster); Temple Gong
  (three pairs at inharmonic ratios, one of them modulated by a fixed 137 Hz operator, so each key grinds
  differently, the modulators dying before their carriers and velocity driving every index); Feedback Lead (a
  mono lead whose oscillator is a self-modulating operator, the mod wheel opening its feedback and index from flute
  to buzzing reed, a mod envelope's bite on each note and a vibrato that a second envelope, as its aux, fades in);
  Six-Op Cascade (all six operators in one chain, F down to A, four slow LFOs breathing its links at rates that
  never line up, and an Index macro deepening them all); Neon Pluck (a mono saw plucked by FM through a
  resonant low-pass over a sine sub, a two-bar LFO turning its FM and grit, its effects as a graph: a split at
  180 Hz whose high band is widened and echoed while the low band stays tight, then a 3-band dynamics set as OTT;
  its Timbre, Filter, Grit and Space macros). Then two more for each category, one in the style of 1980s FM synths
  (sine operators only, indexes falling fast and scaled by velocity) and one modern and heavy: Eighties Slap (a
  1:1 pair with feedback for the body, a 9:1 click for the thumb and a sub sine) and Grind Bass (a fed-back pair
  growled by a sub-octave linear FM through an asymmetric shaper and a driven low-pass, over a clean sub, its effects
  as a graph that keeps the lows below 140 Hz tight while the highs are clipped and widened; its Growl, Drive and
  Width macros); Log Drum (a tuned log drum, a 3:1 knock dying in a blink as its pitch drops into the note, in mono, so
  each hit cuts off the one before, playing a tribal 12/8 groove in eighth triplets while a key is held, an octave
  down: a low call on C minor pentatonic toms answered higher, with a roll down at its end, looped every 24 steps so
  it keeps to two 4/4 bars, its ghost hits softer than its accents) and Hard Kick (a fixed-pitch sine swept down from about 600 Hz into an asymmetric shaper whose tail distorts, under a
  noise click; its Pitch, Distortion, Length and Click macros); Jet Takeoff (a modulator at full feedback, driven
  by an inharmonic one, roaring through its carrier, noise made by FM alone, a slow pitch envelope lifting it two
  octaves under a flanger) and Crush Downer (an FM growl folded by a sine-fold shaper and falling three octaves
  onto the note under band-passed noise falling with it); Funk Clav (a 1:1 pair with feedback for the nasal buzz
  beside a 2:5 pair for the pluck) and Driven Tines (an FM tine piano through an asymmetric shaper, a cabinet's
  band and a stereo phaser, velocity driving the amp too); Classic Synlead (a fed-back 1:1 pair's saw beside a
  2:1 pair's square, detuned, with a bite on each note and a delayed vibrato the mod wheel deepens) and Screamer (a
  square swept by a 2:1 modulator and unison saws, sine-folded and torn through a driven low-pass, the mod wheel
  screaming the fold and feedback up); Vintage Strings (three fed-back pairs bowing in behind their carriers, an
  octave on top, a slow drift on the indexes, chorused into a hall) and Distorted Swell (wide unison saws growled
  by a sub-octave modulator through a driven low-pass into a sine-fold shaper, a slow LFO and a long envelope
  opening it as it is held); Koto Pluck (a fed-back 1:1 pair and a 7:1 snap on one carrier, a 2:3 pair ringing an
  octave up) and Razor Pluck (a fed-back pair driven by a 2:1 modulator through a snapping resonant low-pass over
  a sub and a noise click, hard-clipped, squashed OTT-style and echoed); and Mallet Steps (a marimba, a 1:1
  carrier knocked by a 4:1 modulator, struck in sixteenths by the sequencer while a key is held, through octaves,
  fifths and sevenths, each note's velocity its accent, louder and harder) and Hammer Gate
  (a fed-back pair growled by a sub-octave linear FM through a driven, resonant low-pass into a hard clipper, in
  mono, playing a two-bar Phrygian riff while a key is held, two octaves down, its rests chopping it and octave
  jumps in it, its accents as velocities brightening it and an envelope snapping the cutoff open on each
  note; its Cutoff, Drive and Snap macros); and Future Plucks (a
  carrier knocked by a saw modulator through a low-pass an envelope plucks open, velocity brightening it and two
  Random sources wandering the indexes, playing the sequencer's Phrygian riff while a key is held, an octave down,
  humanized and with chords on some steps, through a chain of EQs, a disperser, compression, chorus, echoes and a
  reverb).
  Then three that play through the Amp: Crunch Rhythm (a string-like FM pluck, two carriers a few cents apart,
  a feedback modulator for the saw's edge and a short bright one for the pick, velocity digging in, through the
  Crunch amp into a closed 4x12 and a small room: a rhythm guitar for chords); Overdrive Lead (a mono, gliding
  lead through the Lead amp into an open 2x12, a vibrato that an envelope fades in as a note holds, the mod wheel
  turning up the amp's Gain, then ping-pong echoes and a plate); and Driven Bass (a round FM bass with a sub an
  octave down, through the Bass amp's grit into a 4x10, its lows staying clean). Then four more that play the
  sequencer: Acid Rubber (a mono saw knocked by a fed-back 1:1 sine through a driven, resonant 24 dB ladder, a
  two-bar C minor line in sixteenths while a key is held, two octaves down, its accents as velocities deepening the
  filter envelope and its slides gliding, the pattern's mod wheel curve opening the filter across the second bar,
  into grit, a dotted echo and a room); Offbeat Stabs (a fed-back FM keys stab through a plucked low-pass, Dorian
  seventh chords on the off-beats and a bass on the downbeats, swung, with the host's transport; pattern B a pushed,
  busier variation; each pattern's bend curve dipping the pitch like tape at its end, through chorus, echoes, a plate
  and a compressor); Random Glass (a glassy 7:1 tone with a twelfth above, its sixteen minor-pentatonic steps
  played in random order, many left to chance, humanized, a Random source stepping the index, into ping-pong echoes
  and a shimmer); and Bender (a soft FM pad, a 1:1 pair with a glassy 2:1 pair an octave up, holding four-voice
  chords in C minor while a key is held, each voice a note of its own whose bend glides it into the next chord's
  voice, some up and some down, four chords to a note, a bar each, in two groups of four bars of eighths, through a
  chorus and a hall). Each release brings
  them as it tunes them. Being built in, they can't be saved over: Save on a changed one saves it as a new preset, as
  Save As does. Versions before 0.12 wrote them out into the folder's `Factory` subfolder: the library leaves the
  files there named as a factory preset out (they stay on disk, for the user to keep or delete), and the subfolder
  too unless the user saved presets of their own into it, which it lists there; a project that named a factory preset
  in it loads the built-in one of that name. So no preset is saved there under a factory preset's name, and no folder
  is made or renamed `Factory` at the top of the preset folder. Their routing graphs are laid out with no node on another, and no wire, line to OUT or
  label running over a node it doesn't join. They hold only the params that differ from Init, so they are small, and
  load like any other preset.
  - **Preset info**: a preset says who made it (**Author**), what kind of sound it is (**Category**) and what it
    does, in a **Description** of one line (up to 80 characters). They are saved with the patch, in its layout chunk,
    so a preset file carries them and so does the host's project; a preset saved before them has none, and still
    loads. The categories are Bass, Drum, FX, Keys, Lead, Pad, Pluck and Seq, each in a color of its own, and any the
    user adds (**+ New** in the Save dialog): a name of 6 characters at most (a built-in one's picking that one) and
    one of five colors,
    kept in the user's settings, so every FMVibe offers them. A preset stores its category by name, so one made on
    another computer with a category this one doesn't know still shows it, in grey. The factory presets are FMVibe's,
    each in one category, with a line about it.
  - The **preset bar** in the middle of the title bar shows the preset loaded or saved last, or **- Init -** (as
    a new instance does), on one line as tall as the title bar's buttons: its category at the left (a dot in its color, then its name), its name in
    the middle (with a dot while the patch has changed since) and "by" its author at the right, before the (i); a name
    too long for the middle moves left, and the author is cut short for it (a preset with neither, and Init, shows its
    name alone).
    Resting the mouse on it shows **Browse Presets** (the description is on the info card). Its arrows step to the previous or next preset in the
    library (the factory presets, then the preset folder's, folder by folder), round from the last to the first.
    The **(i)** at its right (not on Init) opens the preset's **info card** under it: its pill, name, author, folder (or
    Factory) and the tempo it was saved at, its description, a star making it a favorite, **Show in Browser** and
    **Edit Info…** (**Add Info…** while it has none), which opens the Save dialog as Preset Info. A factory preset's card
    says it is read-only and offers **Save Copy…** instead, the Save dialog. Escape, or a click around it, closes it.
  - Clicking the name opens the **browser**, a popup under the preset bar: a search row, then a sidebar beside the
    list, then a strip with the preset picked. It opens
    where it was left, on the sidebar's entry picked last (kept in the user's settings; **Factory** to begin with, and
    when the folder picked has gone), the folder tree opened down to it, with the preset loaded picked; **Show in
    Browser** opens it on the preset's folder.
    - The **sidebar**: **All**, **Favorites** and **Recent** (the presets loaded last, the last first), then
      **FOLDERS**, a tree of **Factory** and **User** (the preset folder) with every subfolder at any depth, each
      opened and closed by its chevron and with how many presets it holds, and its **+**, which makes a new folder in
      the folder shown; a folder's right-click menu (none on Factory) offers **New Folder** (one in it, the browser
      showing the folder it makes one in) and **Open in Explorer**, and on a subfolder **Rename…** (a form holding its
      name; a name another folder there has is refused, and the presets in it, the favorites and recent presets among
      them, go with it) and **Delete…** (asking first, with how many presets go with it, then moving it to the Recycle
      Bin; they leave the favorites and recent presets, and a patch that was one of them stays as it sounds, as an
      edited patch with no preset); then **CATEGORIES**, each with how many of the presets listed are in it (of All, Favorites,
      Recent or the folder picked above, and what the search finds, whatever the categories filter by; the user's own
      after a line, then
      **None**), a click filtering the list by it or taking the filter off (several at once list the presets in any
      of them) and **Clear** taking them all off (a new category is made in the Save dialog). One of the user's own shows an **×** in
      its count's place under the mouse, which removes it from the list after asking (how many presets are in it):
      the presets keep it, nothing written to their files, and it lists among the categories presets have that the
      list doesn't know, in grey, until it is added again. The sidebar scrolls under the wheel, by its scrollbar at its right edge (the thumb dragged, or the track clicked to
      jump there) and dragged with the middle button.
    - The **list** shows the folder's presets, and with **Subfolders** on (the switch in the search row, on to begin
      with, greyed out for All, Favorites, Recent and Factory) its subfolders' too, each then with its folder's name before it; the folder's path over it leads back up,
      a click on a part showing that folder, and the category filters show beside it as chips, a click taking one off.
      Each row has the name (in a light grey near the text's color, standing out from the grey author, and in the text's
      color under the mouse and picked), its description in faint text in the room the name leaves, the category's
      pill (right-aligned in a column as wide as the widest listed), a star (a favorite, or not) and the author; the preset loaded has a
      dot. A click (or a tap) on a preset loads it at once, the browser staying open for the next; a double-click (or
      Enter, for the preset Up, Down or the search picked) loads it and closes the browser. Loading asks first while the
      patch has changed (over the browser, which stays open when the load is called off).
    - The **strip** shows the preset picked: its pill, name, author and folder over its description, then a button
      that shows its file in Explorer, picked (greyed out while nothing is picked, and for a factory preset, which has
      none), and **Close**, which closes the browser as a click around it does. While the sidebar shows **User** or a
      folder in it, **Save Here…** sits beside Close: it opens the Save dialog (see Save As below) with that folder
      picked, and is greyed out when Save As is. Factory, All, Favorites and Recent, which nothing can be saved in,
      don't show it, nor does the browser demo, which has no files. A preset's info is edited from the info card, for
      the preset loaded.
    - Favorites and the recent presets are kept in the user's settings, which every instance shares: what one instance
      changes there leaves what another changed, and shows in the others as their browsers open.
    - A click in its **search**, in its first row, gives it the keyboard, and a click anywhere else takes it away (while
      it hasn't the keyboard, Enter still loads the preset picked and Escape closes the browser): what is typed narrows the
      list (of what the sidebar shows) to the presets whose name, folder, author, category and description hold every
      word of it, whatever the case, and the count at its right end says how many it lists of all of them. The first
      one found is picked; Up and Down pick another from the list (from the preset loaded, before anything is picked),
      and Enter loads the one picked. Escape, or the × in the search, empties it; Escape with nothing typed closes the
      browser, as a click around it or on the preset bar does. The search starts empty each time the browser opens.
      It edits like a text field: Left and Right move the caret, with Ctrl a word at a time (to the start of the word
      before it or after it, words parted by spaces), Home and End jump to the ends, and with Shift they select; a
      click puts the caret there, a drag or a Shift+click selects, a double-click picks a word, and Ctrl+A
      selects it all. Typing replaces the selection, Backspace and Delete take it out, and Ctrl+C, Ctrl+X and Ctrl+V
      copy, cut and paste (line breaks pasted as spaces, up to 40 characters in all). A search longer than the field
      scrolls to keep the caret in sight. A value typed into a knob copies and pastes too, while all of it is
      selected, and its caret steps a word at a time with Ctrl too. The Save dialog's fields edit the same way.
  - The **...** after Init and Save, lit while it is open, opens the **overflow menu** under it, drawn as the
    editor's lists are: **Init preset**, **Load preset...**, **Save preset** and **Save preset as...**, each beside its
    icon (Init's arrow, an open folder, a disk, a disk with a plus), then, after a line, the patch's own tuning (see
    Tuning files): **Load custom tuning...** and **Load keyboard mapping...** (an open folder each), and while there is one,
    a button with Init's arrow at the entry's right end taking it out (**Reset tuning**, **Reset keyboard mapping**,
    as their tooltips say) and a faint line under it, indented past its name, naming the file loaded, the menu growing
    for it, then, after another line, **User manual** (a book, opening
    the user manual in the web browser, as the settings dialog's book does) and **Settings** (a cog). Each does
    what Init's and Save's buttons do, and a greyed-out one (as Save and Save As are below) passes the mouse by. A
    click on an entry, around the menu or on the ... again closes it, as Escape does.
  - **Save As** (a disk with a plus; **Save preset as...** in the overflow menu) opens the **Save dialog**, one column 540 px wide dropping from the preset bar as
    the info card does: **Name** (the preset's, all selected, to type over) and **Author**, **Category** (a chip for
    each, **None**, and **+ New**, which opens the New Category form), **Description** (with how many of its 80
    characters are used) and **Folder**. The author is the preset's (a factory preset's copy's FMVibe); for a preset
    without one, it is the author the user typed into the dialog last (kept in the user's settings, which every
    instance shares, once a preset is saved with an author typed in; empty until then). The folder is a list button
    naming it from User down ("User / Live Set / Act 1"): the preset's own folder to begin with, for a preset of the
    user's; else the folder the browser was left on, if it was left on one; else User (Save Here… picks the folder
    the browser shows). Its list opens under it (over
    it where the editor ends), eight rows at most, scrolling under the wheel: Factory, locked, then User and every
    subfolder, the one picked marked, then **New Folder** under a line, which makes a folder in the one picked and
    picks it. A click on a folder picks it and closes the list; Escape or a click around the list closes it alone.
    Folders are renamed and deleted in the browser. A name the folder already has says so, and the button reads
    **Replace**. Tab moves on to the next text field (Name, Author, Description, then round to Name) and Shift+Tab back
    to the one before, the field's text all selected to type over. Enter or **Save** saves it there, with its info, and
    it is the patch's preset then; Escape, Cancel or the × close it. **Save to File…** saves it anywhere through the
    standard file dialog instead. Editing a preset's info (Edit Info… on the info card) opens the same dialog, titled
    **Preset Info**, its name shown and not edited: Save writes the author, category and description into the
    preset's file and leaves its sound alone (and the patch, if it is that preset, keeps its changes; only its info is
    the file's); another folder picked moves the preset's file there too (a preset of its name there asking to be
    replaced, as Save As does), the patch, a favorite and a recent preset that are that preset following it, and
    "Could not move preset" says when it couldn't, the dialog staying open. In the browser demo, which has no files,
    Save As says so.
  - **Init** sets every param and the routing layout back to their defaults (the zoom stays), the preset's info
    too. **Load**, and Save to File in the Save dialog, read and write a `.fmv` file through the standard file dialog,
    which always starts in the preset folder, wherever the host's last file dialog was left; Save to File suggests the
    current preset's name. The file holds the same data as the host's saved state, apart from the settings.
  - **Save** (the disk, on the title bar and as **Save preset** in the overflow menu) writes the patch over the file of the preset it is, after asking ("Overwrite …?"), and
    tells in a message only if it couldn't (the Save dialog and Edit Info likewise). It is on only for a preset loaded (or saved) that has
    changed since; otherwise it is greyed out and does nothing. On a factory preset, which is built in, and on Init,
    which has no file, it asks nothing and opens the Save dialog instead, as Save As does. Save As is greyed out too on
    Init until the patch changes. In the browser demo, which has no files, both say so.
  - Init and loading ask first only while the patch has changed since it was loaded, saved or reset, or when the
    host brought a patch that isn't a preset, since the patch they replace can't be undone back (undoing every edit
    since a preset loaded leaves it unchanged again). The project remembers which preset it was and whether it changed.
- **Settings** (the overflow menu's last entry), in two tabs: **Visual**, the editor's look (Theme, under its heading, then
  Hue, Glow, Lines, Grid, Handles and Cards), and **Engine**, the instance's settings (Oversampling, MPE and Tuning). The
  dialog opens on Visual and then on the tab shown last while the editor is open, one height for both (Engine's), the version
  and the license at the bottom of each. **Oversampling**, 1x, 2x, 4x or 8x. It is saved with the project but not
  with presets, and changing it restarts the plugin (the host deactivates and reactivates it).
  **MPE**, Off or On, and its **Bend** range for the member channels, 12, 24, 48 (to begin with) or 96 semitones,
  faint while MPE is off (see MPE, above). Like Oversampling, they are saved with the project but not with presets,
  and aren't undo steps.
  **Tuning (MTS-ESP)**, Off (to begin with) or On, and its **Retune**, Note On or Continuous (to begin with), faint
  while MTS-ESP is off (see Microtuning, above), saved likewise; under them, where the tuning comes from: "Connected
  to MTS-ESP:" and the scale's name after a lit dot, "Tuned by MTS SysEx:" and its name after a grey one, "No MTS-ESP
  master. MTS SysEx can retune it too.", or with it off "Off: ignores MTS-ESP masters and MTS SysEx." (the patch's own tuning playing on) It follows a master
  coming and going while the dialog is open. In the browser demo, which has none, the switches are disabled and the
  line says so.
  **Theme**: Spectrum (deep indigo, the default: its window, panels, cards and displays' wells four steps of one
  blue-grey, its text in three greys each at 4.5 : 1 or more on all of them), Porcelain (Spectrum in light: white panels lifted off a cool gray
  by soft shadows, their cards with a slight one too), Midnight or Light (crisp: white cards and knob caps on a cool
  gray, with deep colors), each button shown in its own colors. The browser demo's page around the editor is in
  Spectrum's colors. Its Full screen button, beside the links under the editor, puts the page in full screen with
  the editor alone, as large as the screen takes it in its shape, the button in the screen's top right corner to
  leave it (and a phone turned to landscape where the browser lets the page say so); where the browser can't put a
  page in full screen (an iPhone's Safari) the button isn't shown. Spectrum and Porcelain share a flat look: each
  effect and source card in the cards' plain color, as in every theme (or, with the settings' Cards on Tinted,
  tinted with its own color, faintly while it is switched off), its own color on its icon or handle, its
  knobs, its curves and its lit switches, and outlined only while the mouse is over it (or it is dragged),
  a split's container and its card in the effects graph likewise, on the same shadow as the cards beside them, the
  band it shows faintly in its color behind its cards; a knob's arc, and faintly its track, in the color of what it
  belongs to (its card's; in the inspector a filter's and a shaper's, and the routes into them on an operator's I/O tab, in
  that node's color, as its switches are, an operator's in the accent), modulated or not: a modulation ring whose
  source's color is like the arc's is drawn lighter (in Spectrum) or darker (in Porcelain), keeping its hue, so it
  stands apart from the arc, while the others keep their sources' colors; and its pointer a dot; an LFO's shape filled on
  its card; a lit switch filled solid with its color, rows of them set in a well (the LFO's clock and polarity icons
  a little smaller there; in Spectrum's header, whose dark bottom the well would melt into, on a raised face
  instead), and so the split's 2 | 3, the scope's cycle counts, the Param EQ's band chips (the picked
  one tinted with its band's color), the octave buttons and the dialogs' buttons; list buttons, the lists and menus
  they open, tooltips, the name tags and the messages (on a soft shadow, white in
  Porcelain and a step lighter than the rows in Spectrum, their rows shaded under the mouse), the title bar's
  buttons, the preset's name, the output scope, the Ratio readout (outlined under the mouse), the note source's and
  velocity's curves and the wheels without outlines, and search fields outlined only while typed in or holding
  words; the dialogs lifted by a shadow in Porcelain, as its panels are; + Effect, + Source and the Mods view's + Aux
  and + Add modulation as filled pills, an aux that is set a chip tinted with its source's color, and the cards that
  add an effect, a source or a band's effect filled faintly, a disc behind their plus; each operator, its tab, its
  names in the matrix (whose legend shows the operators' colors side by side) and the routes it sends to another
  operator in a color of its own (a route into or out of a filter or a shaper takes that node's color, as in every
  theme); the routing nodes on a soft shadow (a silent one outlined in a faint tint of its color, and an operator's
  unison copies tinted discs peeking out on both sides, under the wires so an arrowhead coming in at its side shows);
  the effects graph's nodes, a split and a join too, filled
  with a tint of their color, without an outline; and meters and a dimmed control taking the shade of what they are on. Settings
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
  one a handle being dragged trails) are drawn: Off draws none, 100 % draws them opaque. **Grid**, a slider like it
  from Off to 100 % (50 % to begin with, and a double-click puts it back), sets how strongly the Seq view's roll draws
  its grid lines: Off draws none, 100 % twice as strongly as 50 %. With the mouse resting on any of the four sliders
  (not while one is dragged), a tooltip under it says what it sets and that a double-click puts it back. **Handles**, Small (to begin
  with), Medium or Large, draws the depth handles and their **+n** pills bigger, each step about half a pixel more in
  radius and a pixel more across, in the same places (so on an I/O tab label or a cell, Large ones overlap a
  little). **Cards**,
  Plain (to begin with) or Tinted, tints each effect's and source card's face with its own color in Spectrum and
  Porcelain, as those themes drew their cards before they were plain (switched off, faintly); Midnight and Light
  outline their cards in their colors instead, so there its name and buttons are faint and disabled, taking no
  click. All seven are yours rather than the
  project's: they are kept in `%APPDATA%\FMVibe\settings.txt`, every FMVibe editor uses them, and other open editors
  switch along. An editor writes only what it changed into that file, leaving what another one changed meanwhile. The dialog's bottom line shows the version number and that FMVibe is released under the MIT
  license. A book before its x (its tooltip "Open the user manual") opens the user manual online, at the address the
  README links, in the web browser (in the browser demo, a new tab), leaving the dialog open. The editor's size is kept there too (`zoom`), written when an editor you resized closes: a new instance
  opens at it, while a project that saved a size keeps its own. So is whether the keyboard strip is collapsed
  (`keysCollapsed`, see the keyboard strip). A size that doesn't fit the monitor's work area is scaled down to fit,
  its zoom kept.

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

Both report their **latency** (CLAP's latency extension, VST3's `getLatencySamples()`): the voices' decimator's delay
at the oversampling (about 2 samples at 2x, none at 1x) and the effects' latest path's (see Latency, under the effects),
rounded to whole samples, as the plugin was last activated (a VST3 host asking while it is inactive hears what the next
activation will make it). An edit, once it is whole, or a state that moves it asks the host for a restart, whose activate
reports the new one (a CLAP host is told through its latency extension's `changed()`); an edit that leaves it as it was,
an effect moved along the line say, asks for none. While a key, or a sustain pedal, holds a note, the restart a new
latency takes waits until they are all up (a key let go while the plugin is inactive would reach no voice, and its
note would sound on), and is asked for then.

The notes play on through a restart the plugin asked for (the one a new latency asks for, say), when its activate comes
within a second of the deactivate, with no key or pedal holding a note, at the same sample rate and oversampling, with
as many voices allocated as the limit needs or more: the voices (the notes in their release, and the sequencer's), their
envelopes, the sequencer and the effects carry on where they were, and only the latency is new, each note's end
reported once (a CLAP host keeps its note ids). Any other activate (with a key struck since the restart was asked for
still down, for more voices or another oversampling, at another rate, or one the plugin didn't ask for, a device
switched off and on again, say) allocates the voices again and ends the notes playing, each one's end reported in the
next block. A VST3 host stopping processing ends them too, unless a restart was asked for: then they end only if
processing starts again without it.

Hosts are given 2,027 of FMVibe's 52,142 parameters: the globals, the operators, the matrix and the routes' modes,
the filters, the shapers, the macros' knobs, the note source's, the mod wheel's, velocity's, the pitch bend's and the
operators' own as sources, and every modulation's depth and Amount (16 slots for each of the 50 sources, used or not). The rest are set in the editor only:
the effects, the LFOs', envelopes' and other sources' own knobs, what each modulation targets and its aux, curve and
bypass, the Curve sources' drawn shapes and loop regions, the shaper curves and the note source's curve, the Custom
waves' partials and their counts, the effects graph (its splits, joins and wires), the chain's retired order, the feedback's route modes and the
retired fields. They are
saved with the project and in presets like the others, the editor's sources modulate them and its history undoes
them, but hosts can't automate them and don't record their edits: an edit of one marks the project changed
instead.

What the VST3 plugin does its own way:

- **MIDI**: VST3 hands plugins notes, but no controllers. FMVibe asks hosts to map every controller, channel pressure
  and pitch bend, on each of the 16 MIDI channels, to hidden parameters of its own, and takes them as MIDI from
  there: pitch bend (by Bend Range), the MIDI sources' controllers and pressure, and MPE over MIDI work as in the
  CLAP plugin. Polyphonic pressure comes as its own event, a note's pressure. MIDI SysEx comes as data events, whose
  MTS SysEx retunes the synth as in the CLAP plugin (see Microtuning).
- **Note expressions**: Volume, Pan, Tuning (ten octaves either way) and Brightness (a note's slide), as VST3 names
  them, and Pressure, one of FMVibe's own. For touch surfaces and MPE controllers, FMVibe tells hosts to send a
  finger's sideways movement as Tuning, its movement along the key as Brightness and how hard it presses as
  Pressure. A note-on's tuning (in cents) tunes the note from its start.
- **Parameters** are grouped in units, as their modules are (Operators, Op A, Filters, ...).
- **Restarting**: raising the voice limit past the voices allocated (a project loaded may too), changing the
  oversampling or a change to the effects that moves the latency asks the host to deactivate and reactivate the plugin
  (through a latency change, which has hosts do that). A voice limit raised by hand or by the host asks once it has
  settled (see Notes), from a timer on the main thread: CLAP's timer-support, where the host has it (a host without
  it is asked at its next main-thread callback), and in VST3 the plugin's own window's. Stopping processing ends the
  notes, but not on the way to the restart asked for, which the notes in their release play on through as in the CLAP
  plugin.
- **Not in VST3**: CLAP's per-voice and global host modulation, Bitwig's remote-control pages, and the note-end
  reports that per-voice modulation needs.

### On Linux

Both plugins build on Linux too (`./build.sh`): the same engine, parameters and state, the editor an X11 child window
of the host's drawn through Cairo (XWayland included; no Wayland and no GTK is linked). What differs from Windows:

- **File and warning dialogs** are the desktop's own — `zenity`, else `kdialog` — run as separate programs beside the
  host, never inside it. While one is open the editor dims under a veil that says where it went ("A dialog is open",
  "Alt+Tab to show it, Esc to close it"); Escape closes it. With neither tool installed the editor says so for a moment
  ("install zenity or kdialog for file dialogs") and nothing opens; the preset browser's own Save dialog still works.
- **The preset folder** is `$XDG_DATA_HOME/FMVibe/presets` (else `~/.local/share/FMVibe/presets`) and the **settings**
  are `$XDG_CONFIG_HOME/FMVibe/settings.txt` (else `~/.config/FMVibe/settings.txt`). Deleting a preset folder moves it
  to the freedesktop Trash where there is one for it, else deletes it.
- **The editor's clock** is the host's: CLAP's timer-support, VST3's `Linux::IRunLoop` (a 16 ms timer), with a fallback
  where the host offers neither. Only the entry points are exported (`clap_entry`; `GetPluginFactory`, `ModuleEntry`,
  `ModuleExit`), so a CLAP and a VST3 loaded in one process can't bind each other's symbols.
- **The editor's scale** is the host's where it gives one (CLAP's `set_scale`, VST3's content scale factor), else the
  desktop's `Xft.dpi` over 96: the editor opens at that scale, its zoom kept.
- Text is drawn in Inter (`FMVIBE_FONT` picks another family), and typing takes Latin-1 — no compose or IME.
- **MTS-ESP** finds its library at `/usr/local/lib/libMTS.so`, where an MTS-ESP master for Linux installs it. Without
  one, MTS SysEx still tunes the synth.
