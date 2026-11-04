#include "factory.h"

#include "gui/controller.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>

namespace fmvibe {

namespace {

// Builds a preset's changes from Init, an operator, a route or an effect at a time.
struct Patch {
    std::vector<std::pair<int, double>> changes;

    Patch &set(int index, double value) {
        changes.push_back({index, value});
        return *this;
    }
    // Operator `op` switched on at a ratio (Coarse, 0 for 0.5), sending `out` to the output, with its
    // envelope in seconds and its sustain level.
    Patch &op(int op, int coarse, double out, double attack, double decay, double sustain, double release) {
        set(paramIndex(op, OpParam::Enabled), 1.0);
        set(paramIndex(op, OpParam::Coarse), coarse);
        set(paramIndex(op, OpParam::Out), out);
        set(paramIndex(op, OpParam::Attack), normFromTime(std::max(attack, kMinTime)));
        set(paramIndex(op, OpParam::Decay), normFromTime(decay));
        set(paramIndex(op, OpParam::Sustain), sustain);
        return set(paramIndex(op, OpParam::Release), normFromTime(release));
    }
    Patch &opSet(int op, OpParam field, double value) { return set(paramIndex(op, field), value); }
    Patch &mod(int from, int to, double amount) { return set(matrixIndex(from, to), amount); }
    // An effect in `slot`; slots run in their numbers' order, as Init's chain order has them.
    Patch &fx(int slot, FxType type, std::initializer_list<std::pair<FxParam, double>> fields) {
        for (const auto &[field, value] : fields)
            set(paramIndex(slot, field), value);
        return set(paramIndex(slot, FxParam::Type), static_cast<double>(type));
    }
    // An LFO in source place `source`, at `hz` (free of the tempo; synced, it would run in quarter notes),
    // modulating `target` by `amount`.
    Patch &lfo(int source, LfoShape shape, double hz, int target, double amount) {
        set(paramIndex(source, SourceParam::Shape), static_cast<double>(shape));
        set(paramIndex(source, SourceParam::Sync), 0.0);
        set(paramIndex(source, SourceParam::Rate), normFromRate(hz));
        set(paramIndex(source, SourceParam::Type), static_cast<double>(SourceType::Lfo));
        return aim(source, 0, target, amount);
    }
    // A mod envelope in source place `source` (times in seconds), modulating `target` by `amount`.
    Patch &env(int source, double attack, double decay, double sustain, double release, int target, double amount) {
        set(paramIndex(source, SourceParam::Attack), normFromTime(std::max(attack, kMinTime)));
        set(paramIndex(source, SourceParam::Decay), normFromTime(decay));
        set(paramIndex(source, SourceParam::Sustain), sustain);
        set(paramIndex(source, SourceParam::Release), normFromTime(release));
        set(paramIndex(source, SourceParam::Type), static_cast<double>(SourceType::Env));
        return aim(source, 0, target, amount);
    }
    // Another of a source's targets, in its slot `slot`.
    Patch &aim(int source, int slot, int target, double amount) {
        set(slotTargetIndex(source, slot), slotTargetValue(target));
        return set(slotAmountIndex(source, slot), amount);
    }
    Patch &sourceSet(int source, SourceParam field, double value) { return set(paramIndex(source, field), value); }
    // Filter `filter` switched on and going out, its type, cutoff in Hz and resonance; its other fields as Init has
    // them.
    Patch &filter(int filter, FilterType type, double cutoffHz, double resonance) {
        set(paramIndex(filter, FilterParam::Enabled), 1.0);
        set(paramIndex(filter, FilterParam::Out), 1.0);
        set(paramIndex(filter, FilterParam::Type), static_cast<double>(type));
        set(paramIndex(filter, FilterParam::Cutoff), normFromCutoff(cutoffHz));
        return set(paramIndex(filter, FilterParam::Resonance), resonance);
    }
    Patch &filterSet(int filter, FilterParam field, double value) { return set(paramIndex(filter, field), value); }
    // Shaper `shaper` switched on, its curve and drive in dB; its other fields as Init has them.
    Patch &shaper(int shaper, ClipMode mode, double driveDb) {
        set(paramIndex(shaper, ShaperParam::Enabled), 1.0);
        set(paramIndex(shaper, ShaperParam::Mode), static_cast<double>(mode));
        return set(paramIndex(shaper, ShaperParam::Drive), driveDb);
    }
    Patch &shaperSet(int shaper, ShaperParam field, double value) { return set(paramIndex(shaper, field), value); }
    // A drawn shape's points, (x, y, curve) each, into the shape whose point count is at `count`: an LFO's, a shaper's
    // curve or the note source's.
    Patch &shape(int count, const std::vector<std::array<double, 3>> &points) {
        for (size_t n = 0; n < points.size(); ++n) {
            const int point = static_cast<int>(n);
            set(shapeFieldIndex(count, point, PointField::X), points[n][0]);
            set(shapeFieldIndex(count, point, PointField::Y), points[n][1]);
            set(shapeFieldIndex(count, point, PointField::Curve), points[n][2]);
        }
        return set(count, static_cast<double>(points.size()));
    }
};

} // namespace

// Each name starts with two letters for its category, so the presets sort into groups: BA bass, FX effect, KY keys
// (bells and organs too), LD lead (brass too), PD pad (moving textures too), PL pluck, SQ sequence (rhythmic, gated).
std::vector<FactoryPreset> factoryPresets() {
    std::vector<FactoryPreset> presets;

    // KY Electric Piano
    // Two carrier pairs: a soft body whose brightness dies away, and a bell-like tine (ratio 14) on top.
    {
        Patch p;
        p.op(0, 1, 0.8, 0.0, 3.0, 0.0, 0.5);
        p.op(1, 1, 0.0, 0.0, 0.9, 0.1, 0.5);
        p.mod(1, 0, 0.35);
        p.op(2, 1, 0.5, 0.0, 2.5, 0.0, 0.5).opSet(2, OpParam::Detune, 7.0);
        p.op(3, 14, 0.0, 0.0, 0.15, 0.0, 0.1);
        p.mod(3, 2, 0.2);
        p.fx(0, FxType::Reverb, {{FxParam::ReverbSize, 0.45}, {FxParam::ReverbDecay, normFromTime(1.6)},
                                 {FxParam::ReverbMix, 0.2}});
        presets.push_back({"KY Electric Piano", p.changes});
    }
    // BA FM Bass
    // Mono and gliding: a carrier with a sub an octave down, and a modulator with feedback for grit whose
    // brightness falls quickly to a growl.
    {
        Patch p;
        p.set(paramIndex(GlobalParam::Mono), 1.0).set(paramIndex(GlobalParam::Legato), 1.0);
        p.set(paramIndex(GlobalParam::Glide), normFromTime(0.04));
        p.op(0, 1, 0.9, 0.0, 1.5, 0.85, 0.08);
        p.op(1, 1, 0.0, 0.0, 0.35, 0.3, 0.1);
        p.mod(1, 0, 0.5).mod(1, 1, 0.35);
        p.op(2, 0, 0.5, 0.0, 2.0, 1.0, 0.08);
        presets.push_back({"BA FM Bass", p.changes});
    }
    // KY Glass Bell
    // Ratios of 3.5 and 7.5 make inharmonic partials that ring out long, in a large room. The two stacks stand side
    // by side, each modulator above its carrier, so no route runs behind another node.
    {
        Patch p;
        p.op(0, 1, 0.7, 0.0, 4.0, 0.0, 2.5);
        p.op(1, 3, 0.0, 0.0, 2.5, 0.0, 2.0).opSet(1, OpParam::Fine, 1.0 / 6.0);
        p.mod(1, 0, 0.45);
        p.op(2, 1, 0.35, 0.0, 3.0, 0.0, 2.0).opSet(2, OpParam::Detune, 4.0);
        p.op(3, 7, 0.0, 0.0, 1.0, 0.0, 0.8).opSet(3, OpParam::Fine, 1.0 / 14.0);
        p.mod(3, 2, 0.3);
        p.fx(0, FxType::Reverb, {{FxParam::ReverbSize, 0.8}, {FxParam::ReverbDecay, normFromTime(4.5)},
                                 {FxParam::ReverbDamping, 0.3}, {FxParam::ReverbMix, 0.35}});
        presets.push_back({"KY Glass Bell", p.changes, {{0.3f, 0.55f}, {0.3f, 0.1f}, {0.7f, 0.55f}, {0.7f, 0.1f}}});
    }
    // PD Warm Pad
    // Four detuned copies fading in slowly, a gentle second harmonic that an LFO lets breathe.
    {
        Patch p;
        p.op(0, 1, 0.7, 0.9, 2.0, 1.0, 1.8);
        p.opSet(0, OpParam::Unison, 4.0).opSet(0, OpParam::UnisonDetune, 14.0).opSet(0, OpParam::UnisonSpread, 0.8);
        p.op(1, 2, 0.0, 1.2, 3.0, 0.7, 2.0);
        p.mod(1, 0, 0.22);
        p.lfo(0, LfoShape::Sine, 0.25, matrixIndex(1, 0), 0.24);
        p.fx(0, FxType::Reverb, {{FxParam::ReverbSize, 0.85}, {FxParam::ReverbDecay, normFromTime(5.0)},
                                 {FxParam::ReverbMix, 0.35}});
        presets.push_back({"PD Warm Pad", p.changes});
    }
    // KY Drawbar Organ
    // Drawbars: sine partials at 0.5, 1, 2, 3 and 4 times the note, keyed on and off at once, a touch of
    // feedback, and a slow phaser for some movement. The five carriers stand in a row, lowest partial first, so no
    // route runs behind another node.
    {
        Patch p;
        const int ratios[] = {1, 0, 2, 3, 4};
        const double levels[] = {0.55, 0.45, 0.35, 0.25, 0.2};
        for (int op = 0; op < 5; ++op)
            p.op(op, ratios[op], levels[op], 0.004, 1.0, 1.0, 0.04);
        p.mod(0, 0, 0.12);
        p.fx(0, FxType::Phaser, {{FxParam::PhaserRate, normFromRate(0.7)}, {FxParam::PhaserDepth, 0.4},
                                 {FxParam::PhaserFeedback, 0.2}, {FxParam::PhaserMix, 0.7}});
        p.fx(1, FxType::Reverb, {{FxParam::ReverbMix, 0.15}});
        presets.push_back({"KY Drawbar Organ", p.changes, {{0.3f, 0.4f}, {0.1f, 0.4f}, {0.5f, 0.4f}, {0.7f, 0.4f}, {0.9f, 0.4f}}});
    }
    // PL Pluck Delay
    // A short, bright pluck into dotted-eighth echoes.
    {
        Patch p;
        p.op(0, 1, 0.8, 0.0, 0.7, 0.0, 0.25);
        p.op(1, 2, 0.0, 0.0, 0.12, 0.0, 0.1);
        p.mod(1, 0, 0.45);
        p.fx(0, FxType::Delay, {{FxParam::DelayFeedback, 0.35}, {FxParam::DelayTone, -0.3},
                                {FxParam::DelayMix, 0.22}});
        p.fx(1, FxType::Reverb, {{FxParam::ReverbMix, 0.15}});
        presets.push_back({"PL Pluck Delay", p.changes});
    }
    // Later examples, after the first kFirstExamples.

    // PD Crystal Motion
    // Everything at once: three layers that move on their own. FM keys (B into A) with a bright bite from a
    // mod envelope; a slow pad (D into C, three detuned triangles) through a 24 dB low-pass that a slow LFO
    // sweeps; and a shimmer of inharmonic partials (F at 7.5 with feedback, into E at 4) through a band-pass
    // that wobbles in eighth notes, while a smooth random LFO varies its brightness. Then a soft shaper,
    // gentle compression, a cut below and air above, a chorusing flanger, ping-pong echoes and a large room.
    {
        Patch p;
        p.op(0, 1, 0.45, 0.005, 2.5, 0.6, 1.2);
        p.op(1, 2, 0.0, 0.0, 1.2, 0.35, 1.0);
        p.mod(1, 0, 0.28);
        p.op(2, 1, 0.0, 0.35, 3.0, 0.8, 1.8);
        p.opSet(2, OpParam::Wave, static_cast<double>(OpWave::Triangle)).opSet(2, OpParam::Detune, 5.0);
        p.opSet(2, OpParam::Unison, 3.0).opSet(2, OpParam::UnisonDetune, 16.0).opSet(2, OpParam::UnisonSpread, 0.9);
        p.op(3, 3, 0.0, 0.5, 2.5, 0.5, 1.5);
        p.mod(3, 2, 0.2).mod(1, 2, 0.06);
        p.op(4, 4, 0.12, 0.0, 1.4, 0.0, 0.9);
        p.op(5, 7, 0.0, 0.0, 0.5, 0.0, 0.4).opSet(5, OpParam::Fine, 1.0 / 14.0);
        p.mod(5, 4, 0.3).mod(5, 5, 0.12);

        p.filter(0, FilterType::LowPass, 1400.0, 0.35).filterSet(0, FilterParam::Drive, 0.15);
        p.filterSet(0, FilterParam::Slope, 3.0); // 24 dB
        p.filterSet(0, FilterParam::KeyTrack, 0.5);
        p.set(sendIndex(2, 0), 0.75);
        p.filter(1, FilterType::BandPass, 2600.0, 0.45).filterSet(1, FilterParam::KeyTrack, 0.3);
        p.filterSet(1, FilterParam::Out, 0.8);
        p.set(sendIndex(4, 1), 0.3);

        // A slow sweep of the pad's filter and FM, on one clock for every voice.
        p.lfo(0, LfoShape::Sine, 0.12, paramIndex(0, FilterParam::Cutoff), 0.24).sourceSet(0, SourceParam::Free, 1.0);
        p.aim(0, 1, matrixIndex(3, 2), 0.16);
        // The shimmer's brightness and band wandering at random.
        p.lfo(1, LfoShape::Random, 0.8, matrixIndex(5, 4), 0.2).aim(1, 1, paramIndex(1, FilterParam::Cutoff), 0.12);
        p.sourceSet(1, SourceParam::RandomSmooth, 1.0).sourceSet(1, SourceParam::RandomChaos, 0.6);
        // The band-pass opening in eighth notes (division 16 is 1/8).
        p.lfo(2, LfoShape::Triangle, 2.0, paramIndex(1, FilterParam::Cutoff), 0.18);
        p.sourceSet(2, SourceParam::Sync, 1.0).sourceSet(2, SourceParam::Division, 16.0);
        p.sourceSet(2, SourceParam::Unipolar, 1.0);
        // A gentle vibrato on the pad alone.
        p.lfo(3, LfoShape::Sine, 4.5, paramIndex(2, OpParam::Detune), 0.12);
        // The keys' bite: more FM and a brighter pad filter as each note starts.
        p.env(4, 0.0, 0.6, 0.0, 0.3, matrixIndex(1, 0), 0.2).aim(4, 1, paramIndex(0, FilterParam::Cutoff), 0.1);

        p.fx(0, FxType::Shaper, {{FxParam::ShaperMode, static_cast<double>(ClipMode::Soft)}, {FxParam::ShaperDrive, 4.0},
                                  {FxParam::ShaperLink, 0.0}, {FxParam::ShaperOutput, -2.0}, {FxParam::ShaperMix, 0.6}});
        p.fx(1, FxType::Compressor, {{FxParam::CompThreshold, -16.0}, {FxParam::CompRatio, 2.5},
                                     {FxParam::CompAttack, normFromTime(0.02)},
                                     {FxParam::CompRelease, normFromTime(0.25)}, {FxParam::CompMakeup, 2.0}});
        p.fx(2, FxType::ParamEq, {{FxParam::PeqType1, static_cast<double>(PeqType::LowCut)},
                                  {FxParam::PeqFreq1, normFromCutoff(90.0)},
                                  {FxParam::PeqType2, static_cast<double>(PeqType::HighShelf)},
                                  {FxParam::PeqFreq2, normFromCutoff(7000.0)}, {FxParam::PeqGain2, 2.5},
                                  {FxParam::PeqQ2, normFromQ(0.7)}});
        p.fx(3, FxType::Flanger, {{FxParam::FlangerRate, normFromRate(0.2)}, {FxParam::FlangerDepth, 0.5},
                                  {FxParam::FlangerDelay, normFromTime(0.006)}, {FxParam::FlangerFeedback, 0.15},
                                  {FxParam::FlangerMix, 0.7}});
        p.fx(4, FxType::Delay, {{FxParam::DelayDivisionL, 15.0}, {FxParam::DelayLink, 1.0}, // 1/8D
                                {FxParam::DelayPingPong, 1.0}, {FxParam::DelayFeedback, 0.35},
                                {FxParam::DelayTone, -0.2}, {FxParam::DelayMix, 0.2}});
        p.fx(5, FxType::Reverb, {{FxParam::ReverbSize, 0.85}, {FxParam::ReverbDecay, normFromTime(4.0)},
                                 {FxParam::ReverbDamping, 0.45}, {FxParam::ReverbPreDelay, normFromTime(0.03)},
                                 {FxParam::ReverbMix, 0.3}});
        // Each stack in a column, its modulator above its carrier: keys, pad over its low-pass, shimmer
        // beside its band-pass.
        presets.push_back({"PD Crystal Motion",
                           p.changes,
                           {{0.18f, 0.55f}, {0.18f, 0.1f}, {0.5f, 0.55f}, {0.5f, 0.1f}, {0.82f, 0.55f},
                            {0.82f, 0.1f}, {0.42f, 1.0f}, {0.68f, 1.0f}}});
    }
    // BA Reese Bass
    // A heavy drum and bass reese, mono and gliding: two stacks of detuned saws (A in four, B in two, a few cents
    // apart) beating against each other through a driven 24 dB low-pass, which a slow LFO and a short envelope
    // move; a clean sine an octave down for the sub; and D growling into the saws, more of it every half note.
    // Then hard clipping, three-band compression, a cut below 30 Hz and less fizz up top, and a slow phaser.
    {
        Patch p;
        p.set(paramIndex(GlobalParam::Mono), 1.0).set(paramIndex(GlobalParam::Legato), 1.0);
        p.set(paramIndex(GlobalParam::Glide), normFromTime(0.06));
        for (int op = 0; op < 2; ++op) {
            p.op(op, 1, 0.0, 0.003, 1.0, 1.0, 0.12);
            p.opSet(op, OpParam::Wave, static_cast<double>(OpWave::Saw)).opSet(op, OpParam::PhaseRandom, 1.0);
        }
        p.opSet(0, OpParam::Unison, 4.0).opSet(0, OpParam::UnisonDetune, 22.0).opSet(0, OpParam::UnisonSpread, 0.5);
        p.opSet(1, OpParam::Detune, -9.0).opSet(1, OpParam::Unison, 2.0).opSet(1, OpParam::UnisonDetune, 12.0);
        p.op(2, 0, 0.45, 0.003, 1.0, 1.0, 0.1);
        p.op(3, 1, 0.0, 0.003, 0.8, 0.6, 0.12);
        p.mod(3, 0, 0.12).mod(3, 1, 0.08);

        p.filter(0, FilterType::LowPass, 520.0, 0.25).filterSet(0, FilterParam::Drive, 0.45);
        p.filterSet(0, FilterParam::Slope, 3.0); // 24 dB
        p.filterSet(0, FilterParam::KeyTrack, 0.35);
        p.set(sendIndex(0, 0), 0.8).set(sendIndex(1, 0), 0.6);

        // The filter opening and the saws' beating speeding up and slowing down, on one clock for every voice.
        p.lfo(0, LfoShape::Sine, 0.25, paramIndex(0, FilterParam::Cutoff), 0.18).sourceSet(0, SourceParam::Free, 1.0);
        p.aim(0, 1, paramIndex(0, OpParam::UnisonDetune), 0.16);
        // The growl swelling every half note (division 10 is 1/2).
        p.lfo(1, LfoShape::Triangle, 2.0, matrixIndex(3, 0), 0.16);
        p.sourceSet(1, SourceParam::Sync, 1.0).sourceSet(1, SourceParam::Division, 10.0);
        // Each note's bite: the filter opens and closes back most of the way.
        p.env(2, 0.0, 0.35, 0.25, 0.2, paramIndex(0, FilterParam::Cutoff), 0.14);

        p.fx(0, FxType::Shaper, {{FxParam::ShaperMode, static_cast<double>(ClipMode::Asymmetric)},
                                  {FxParam::ShaperDrive, 9.0}, {FxParam::ShaperLink, 0.0}, {FxParam::ShaperOutput, -4.0},
                                  {FxParam::ShaperMix, 0.7}});
        p.fx(1, FxType::MultiComp, {{FxParam::McLowFreq, normFromCutoff(180.0)},
                                    {FxParam::McThreshold1, -24.0}, {FxParam::McRatio1, 4.0}, {FxParam::McGain1, 2.0},
                                    {FxParam::McThreshold2, -28.0}, {FxParam::McRatio2, 5.0}, {FxParam::McGain2, 3.0},
                                    {FxParam::McThreshold3, -26.0}, {FxParam::McRatio3, 4.0}, {FxParam::McGain3, 2.0},
                                    {FxParam::McAttack, normFromTime(0.005)}, {FxParam::McRelease, normFromTime(0.12)},
                                    {FxParam::McMix, 0.8}});
        p.fx(2, FxType::ParamEq, {{FxParam::PeqType1, static_cast<double>(PeqType::LowCut)},
                                  {FxParam::PeqFreq1, normFromCutoff(30.0)},
                                  {FxParam::PeqType2, static_cast<double>(PeqType::HighShelf)},
                                  {FxParam::PeqFreq2, normFromCutoff(5000.0)}, {FxParam::PeqGain2, -3.0}});
        p.fx(3, FxType::Phaser, {{FxParam::PhaserRate, normFromRate(0.15)}, {FxParam::PhaserDepth, 0.5},
                                 {FxParam::PhaserCenter, normFromCutoff(900.0)}, {FxParam::PhaserFeedback, 0.3},
                                 {FxParam::PhaserStages, 6.0}, {FxParam::PhaserMix, 0.4}});
        // The saw stacks side by side over their filter, the sub off to the right, the growl above.
        presets.push_back({"BA Reese Bass",
                           p.changes,
                           {{0.2f, 0.55f}, {0.45f, 0.55f}, {0.8f, 0.55f}, {0.32f, 0.1f}, {0.6f, 0.1f},
                            {0.85f, 0.1f}, {0.32f, 1.0f}, {0.65f, 1.0f}}});
    }
    // BA Wobble Bass
    // A dubstep wobble: two detuned saws growling with FM through a driven, resonant 24 dB low-pass that an
    // eighth-note LFO throws open, opening the FM with it; a clean sine an octave down underneath. Then linked
    // clipping, three-band compression, a low cut and a small room.
    {
        Patch p;
        p.set(paramIndex(GlobalParam::Mono), 1.0).set(paramIndex(GlobalParam::Legato), 1.0);
        p.set(paramIndex(GlobalParam::Glide), normFromTime(0.03));
        p.op(0, 1, 0.0, 0.003, 1.0, 1.0, 0.1);
        p.opSet(0, OpParam::Wave, static_cast<double>(OpWave::Saw)).opSet(0, OpParam::Unison, 2.0);
        p.opSet(0, OpParam::UnisonDetune, 8.0).opSet(0, OpParam::PhaseRandom, 1.0);
        p.op(1, 1, 0.0, 0.003, 1.0, 0.8, 0.1);
        p.op(2, 0, 0.4, 0.003, 1.0, 1.0, 0.1);
        p.op(3, 2, 0.0, 0.003, 0.6, 0.5, 0.1);
        p.mod(1, 0, 0.35).mod(3, 1, 0.25);
        p.filter(0, FilterType::LowPass, 300.0, 0.55).filterSet(0, FilterParam::Slope, 3.0);
        p.filterSet(0, FilterParam::Drive, 0.5).filterSet(0, FilterParam::KeyTrack, 0.3);
        p.set(sendIndex(0, 0), 1.0);
        // The wobble: eighth notes (division 16), the filter and the growl together.
        p.lfo(0, LfoShape::Sine, 2.0, paramIndex(0, FilterParam::Cutoff), 0.35);
        p.sourceSet(0, SourceParam::Sync, 1.0).sourceSet(0, SourceParam::Division, 16.0);
        p.sourceSet(0, SourceParam::Unipolar, 1.0).aim(0, 1, matrixIndex(1, 0), 0.25);
        // A little wandering in the resonance, and a pluck on each note.
        p.lfo(1, LfoShape::Random, 0.4, paramIndex(0, FilterParam::Resonance), 0.2);
        p.sourceSet(1, SourceParam::RandomSmooth, 1.0);
        p.env(2, 0.0, 0.15, 0.0, 0.1, paramIndex(0, FilterParam::Cutoff), 0.1);

        p.fx(0, FxType::Shaper, {{FxParam::ShaperMode, static_cast<double>(ClipMode::Asymmetric)},
                                  {FxParam::ShaperDrive, 12.0}, {FxParam::ShaperLink, 1.0}, {FxParam::ShaperOutput, 0.0},
                                  {FxParam::ShaperMix, 0.8}});
        p.fx(1, FxType::MultiComp, {{FxParam::McThreshold1, -22.0}, {FxParam::McRatio1, 4.0},
                                    {FxParam::McThreshold2, -26.0}, {FxParam::McRatio2, 4.0}, {FxParam::McGain2, 2.0},
                                    {FxParam::McThreshold3, -24.0}, {FxParam::McRatio3, 4.0}});
        p.fx(2, FxType::ParamEq, {{FxParam::PeqType1, static_cast<double>(PeqType::LowCut)},
                                  {FxParam::PeqFreq1, normFromCutoff(30.0)}});
        p.fx(3, FxType::Reverb, {{FxParam::ReverbType, static_cast<double>(ReverbType::Room)},
                                 {FxParam::ReverbSize, 0.3}, {FxParam::ReverbDecay, normFromTime(0.8)},
                                 {FxParam::ReverbMix, 0.1}});
        presets.push_back({"BA Wobble Bass",
                           p.changes,
                           {{0.25f, 0.55f}, {0.25f, 0.1f}, {0.75f, 0.55f}, {0.5f, 0.1f}, {0.6f, 0.55f},
                            {0.85f, 0.1f}, {0.25f, 1.0f}, {0.65f, 1.0f}}});
    }
    // PD Shimmer Pad
    // A slow, wide pad made for the shimmer reverb: five detuned triangles through a 12 dB low-pass, an octave
    // below in sine, and an airy saw stack through a band-pass. Three LFOs keep it moving: a slow sweep of both
    // filters, breathing unison detune and a wandering FM. Then a phaser, a low cut, ping-pong echoes and the
    // long shimmering hall.
    {
        Patch p;
        p.op(0, 1, 0.0, 0.45, 2.0, 0.9, 3.0);
        p.opSet(0, OpParam::Wave, static_cast<double>(OpWave::Triangle)).opSet(0, OpParam::Unison, 5.0);
        p.opSet(0, OpParam::UnisonDetune, 18.0).opSet(0, OpParam::UnisonSpread, 1.0);
        p.op(1, 2, 0.0, 0.6, 2.5, 0.7, 3.0);
        p.op(2, 0, 0.3, 0.5, 2.0, 0.9, 3.0);
        p.op(3, 3, 0.0, 0.8, 3.0, 0.6, 3.0).opSet(3, OpParam::Fine, 0.01);
        p.op(4, 2, 0.0, 0.5, 2.0, 0.8, 3.0).opSet(4, OpParam::Wave, static_cast<double>(OpWave::Saw));
        p.opSet(4, OpParam::Unison, 3.0).opSet(4, OpParam::UnisonDetune, 22.0).opSet(4, OpParam::UnisonSpread, 1.0);
        p.mod(1, 0, 0.15).mod(3, 2, 0.1);
        p.filter(0, FilterType::LowPass, 2500.0, 0.2).filterSet(0, FilterParam::KeyTrack, 0.5);
        p.filter(1, FilterType::BandPass, 2000.0, 0.6).filterSet(1, FilterParam::Out, 0.7);
        p.set(sendIndex(0, 0), 0.8).set(sendIndex(4, 1), 0.3);
        p.lfo(0, LfoShape::Sine, 0.08, paramIndex(0, FilterParam::Cutoff), 0.3).sourceSet(0, SourceParam::Free, 1.0);
        p.aim(0, 1, paramIndex(1, FilterParam::Cutoff), 0.4);
        p.lfo(1, LfoShape::Triangle, 0.3, paramIndex(0, OpParam::UnisonDetune), 0.16);
        p.lfo(2, LfoShape::Random, 0.5, matrixIndex(1, 0), 0.16).sourceSet(2, SourceParam::RandomSmooth, 1.0);

        p.fx(0, FxType::Phaser, {{FxParam::PhaserRate, normFromRate(0.1)}, {FxParam::PhaserDepth, 0.6},
                                 {FxParam::PhaserMix, 0.6}});
        p.fx(1, FxType::ParamEq, {{FxParam::PeqType1, static_cast<double>(PeqType::LowCut)},
                                  {FxParam::PeqFreq1, normFromCutoff(80.0)}});
        p.fx(2, FxType::Delay, {{FxParam::DelayDivisionL, 12.0}, {FxParam::DelayLink, 1.0}, // 1/4D
                                {FxParam::DelayPingPong, 1.0}, {FxParam::DelayFeedback, 0.4},
                                {FxParam::DelayTone, -0.3}, {FxParam::DelayMix, 0.15}});
        p.fx(3, FxType::Reverb, {{FxParam::ReverbType, static_cast<double>(ReverbType::Shimmer)},
                                 {FxParam::ReverbSize, 0.9}, {FxParam::ReverbDecay, normFromTime(7.0)},
                                 {FxParam::ReverbDamping, 0.35}, {FxParam::ReverbMix, 0.45}});
        presets.push_back({"PD Shimmer Pad",
                           p.changes,
                           {{0.2f, 0.55f}, {0.2f, 0.1f}, {0.5f, 0.55f}, {0.5f, 0.1f}, {0.8f, 0.55f},
                            {0.8f, 0.1f}, {0.2f, 1.0f}, {0.8f, 1.0f}}});
    }
    // SQ Trance Gate
    // A trance supersaw, gated in sixteenths: seven detuned saws and three an octave up, a touch of FM, through
    // a 24 dB low-pass whose output a square LFO chops while a saw LFO sweeps its cutoff over two bars. Then
    // compression, more air, dotted-eighth ping-pong echoes and a plate.
    {
        Patch p;
        p.op(0, 1, 0.0, 0.005, 1.0, 1.0, 0.3);
        p.opSet(0, OpParam::Wave, static_cast<double>(OpWave::Saw)).opSet(0, OpParam::Unison, 7.0);
        p.opSet(0, OpParam::UnisonDetune, 25.0).opSet(0, OpParam::UnisonSpread, 0.9);
        p.op(1, 2, 0.0, 0.005, 1.0, 1.0, 0.3).opSet(1, OpParam::Wave, static_cast<double>(OpWave::Saw));
        p.opSet(1, OpParam::Detune, 5.0).opSet(1, OpParam::Unison, 3.0).opSet(1, OpParam::UnisonDetune, 15.0);
        p.op(2, 1, 0.0, 0.005, 0.8, 0.6, 0.3);
        p.mod(2, 1, 0.1);
        p.filter(0, FilterType::LowPass, 3000.0, 0.3).filterSet(0, FilterParam::Slope, 3.0);
        p.filterSet(0, FilterParam::KeyTrack, 0.5);
        p.set(sendIndex(0, 0), 1.0).set(sendIndex(1, 0), 0.5);
        // The gate: a square in sixteenths (division 19) turning the filter's output down while it is high.
        p.lfo(0, LfoShape::Square, 2.0, paramIndex(0, FilterParam::Out), -0.9);
        p.sourceSet(0, SourceParam::Sync, 1.0).sourceSet(0, SourceParam::Division, 19.0);
        p.sourceSet(0, SourceParam::Unipolar, 1.0);
        // The sweep: a rising saw over two bars (division 4 is 2/1), on one clock for every voice.
        p.lfo(1, LfoShape::Saw, 1.0, paramIndex(0, FilterParam::Cutoff), 0.25).sourceSet(1, SourceParam::Free, 1.0);
        p.sourceSet(1, SourceParam::Sync, 1.0).sourceSet(1, SourceParam::Division, 4.0);
        p.sourceSet(1, SourceParam::Unipolar, 1.0);
        p.env(2, 0.0, 0.08, 0.0, 0.1, paramIndex(0, FilterParam::Cutoff), 0.15);

        p.fx(0, FxType::Compressor, {{FxParam::CompThreshold, -18.0}, {FxParam::CompRatio, 3.0},
                                     {FxParam::CompMakeup, 3.0}});
        p.fx(1, FxType::ParamEq, {{FxParam::PeqType1, static_cast<double>(PeqType::LowCut)},
                                  {FxParam::PeqFreq1, normFromCutoff(120.0)},
                                  {FxParam::PeqType2, static_cast<double>(PeqType::HighShelf)},
                                  {FxParam::PeqFreq2, normFromCutoff(6000.0)}, {FxParam::PeqGain2, 2.0}});
        p.fx(2, FxType::Delay, {{FxParam::DelayDivisionL, 15.0}, {FxParam::DelayLink, 1.0}, // 1/8D
                                {FxParam::DelayPingPong, 1.0}, {FxParam::DelayFeedback, 0.35},
                                {FxParam::DelayMix, 0.25}});
        p.fx(3, FxType::Reverb, {{FxParam::ReverbType, static_cast<double>(ReverbType::Plate)},
                                 {FxParam::ReverbDecay, normFromTime(2.5)}, {FxParam::ReverbMix, 0.25}});
        presets.push_back({"SQ Trance Gate",
                           p.changes,
                           {{0.25f, 0.55f}, {0.6f, 0.55f}, {0.6f, 0.1f}, {0.5f, 0.1f}, {0.2f, 0.55f},
                            {0.8f, 0.55f}, {0.42f, 1.0f}, {0.65f, 1.0f}}});
    }
    // LD FM Brass
    // FM brass: two stacks a few cents apart, each modulator coming in a little after its carrier so the note
    // blats in; a third operator brightens the first stack's modulator. An
    // envelope pushes the FM at the start and an LFO adds vibrato. Then compression, a presence peak and a hall.
    {
        Patch p;
        p.op(0, 1, 0.6, 0.06, 0.6, 0.8, 0.25);
        p.op(1, 1, 0.0, 0.08, 0.5, 0.6, 0.25);
        p.op(2, 1, 0.35, 0.06, 0.6, 0.8, 0.25).opSet(2, OpParam::Detune, 6.0);
        p.op(3, 1, 0.0, 0.1, 0.5, 0.6, 0.25);
        p.op(4, 3, 0.0, 0.05, 0.3, 0.3, 0.2);
        p.mod(1, 0, 0.45).mod(3, 2, 0.35).mod(4, 1, 0.08);
        p.env(0, 0.0, 0.15, 0.0, 0.2, matrixIndex(1, 0), 0.15).aim(0, 1, matrixIndex(3, 2), 0.12);
        // Vibrato: a few cents either way on both carriers.
        p.lfo(1, LfoShape::Sine, 5.2, paramIndex(0, OpParam::Transpose), 0.0024);
        p.aim(1, 1, paramIndex(2, OpParam::Transpose), 0.0024);

        p.fx(0, FxType::Compressor, {{FxParam::CompThreshold, -16.0}, {FxParam::CompRatio, 2.5},
                                     {FxParam::CompMakeup, 2.0}});
        p.fx(1, FxType::ParamEq, {{FxParam::PeqFreq1, normFromCutoff(1500.0)}, {FxParam::PeqGain1, 3.0},
                                  {FxParam::PeqType2, static_cast<double>(PeqType::LowCut)},
                                  {FxParam::PeqFreq2, normFromCutoff(70.0)}});
        p.fx(2, FxType::Reverb, {{FxParam::ReverbDecay, normFromTime(2.2)}, {FxParam::ReverbMix, 0.2}});
        presets.push_back({"LD FM Brass",
                           p.changes,
                           {{0.3f, 0.55f}, {0.3f, 0.1f}, {0.7f, 0.55f}, {0.7f, 0.1f}, {0.5f, 0.1f},
                            {0.85f, 0.55f}, {0.35f, 1.0f}, {0.65f, 1.0f}}});
    }
    // FX Industrial Hit
    // A percussive industrial hit: a sine body whose pitch drops two octaves in a tenth of a second, clicked
    // by a square; noise through a band-pass; and metallic partials (7.5 into 3.5). Then a sine-fold shaper,
    // fast compression, a low cut and a small room.
    {
        Patch p;
        p.op(0, 1, 0.7, 0.0, 0.4, 0.0, 0.3);
        p.op(1, 1, 0.25, 0.0, 0.18, 0.0, 0.15).opSet(1, OpParam::Wave, static_cast<double>(OpWave::Noise));
        p.opSet(1, OpParam::NoiseSlope, 0.3);
        p.op(2, 3, 0.2, 0.0, 0.3, 0.0, 0.2).opSet(2, OpParam::Fine, 1.0 / 6.0);
        p.op(3, 7, 0.0, 0.0, 0.1, 0.0, 0.1).opSet(3, OpParam::Fine, 1.0 / 14.0);
        p.op(4, 1, 0.0, 0.0, 0.05, 0.0, 0.05).opSet(4, OpParam::Wave, static_cast<double>(OpWave::Square));
        p.mod(3, 2, 0.4).mod(4, 0, 0.15);
        p.filter(1, FilterType::BandPass, 3000.0, 0.5);
        p.set(sendIndex(1, 1), 0.4);
        // The drop: the body starts two octaves up (24 of the Pitch knob's 96 semitones) and falls.
        p.env(0, 0.0, 0.12, 0.0, 0.1, paramIndex(0, OpParam::Transpose), 0.25);

        p.fx(0, FxType::Shaper, {{FxParam::ShaperMode, static_cast<double>(ClipMode::SineFold)},
                                  {FxParam::ShaperDrive, 10.0}, {FxParam::ShaperLink, 1.0}, {FxParam::ShaperOutput, 3.5},
                                  {FxParam::ShaperMix, 0.5}});
        p.fx(1, FxType::Compressor, {{FxParam::CompThreshold, -20.0}, {FxParam::CompRatio, 6.0},
                                     {FxParam::CompAttack, normFromTime(0.002)},
                                     {FxParam::CompRelease, normFromTime(0.08)}, {FxParam::CompMakeup, 4.0}});
        p.fx(2, FxType::ParamEq, {{FxParam::PeqType1, static_cast<double>(PeqType::LowCut)},
                                  {FxParam::PeqFreq1, normFromCutoff(35.0)}});
        p.fx(3, FxType::Reverb, {{FxParam::ReverbType, static_cast<double>(ReverbType::Room)},
                                 {FxParam::ReverbSize, 0.3}, {FxParam::ReverbDecay, normFromTime(0.8)},
                                 {FxParam::ReverbMix, 0.2}});
        presets.push_back({"FX Industrial Hit",
                           p.changes,
                           {{0.2f, 0.55f}, {0.45f, 0.55f}, {0.75f, 0.55f}, {0.75f, 0.1f}, {0.2f, 0.1f},
                            {0.9f, 0.1f}, {0.3f, 1.0f}, {0.55f, 1.0f}}});
    }
    // LD Folded Lead
    // A mono lead whose modulator goes through a shaper before it reaches the carrier: B's sine, folded back on
    // itself by shaper 1, moves A with a brighter, vocal edge that a sine alone can't. An envelope drives the fold
    // hard at each note and lets it settle, a slow LFO keeps it turning, and an octave below in sine gives it body.
    // Then dotted-eighth echoes and a small plate.
    {
        Patch p;
        p.set(paramIndex(GlobalParam::Mono), 1.0).set(paramIndex(GlobalParam::Legato), 1.0);
        p.set(paramIndex(GlobalParam::Glide), normFromTime(0.04));
        p.op(0, 1, 0.7, 0.005, 1.0, 1.0, 0.25);
        p.op(1, 1, 0.0, 0.005, 1.0, 1.0, 0.25);
        p.op(2, 0, 0.25, 0.005, 1.0, 1.0, 0.25);
        p.shaper(0, ClipMode::SineFold, 6.0).set(shaperSendIndex(1, 0), 1.0).set(shaperModIndex(0, 0), 0.45);
        // The fold: up by a third of the Drive knob's 36 dB as a note starts, down to a tenth of it held.
        p.env(0, 0.0, 0.6, 0.3, 0.3, paramIndex(0, ShaperParam::Drive), 0.3);
        p.lfo(1, LfoShape::Sine, 0.25, paramIndex(0, ShaperParam::Drive), 0.12);
        // A little vibrato: 20 cents of swing on each operator's Detune, so they keep their ratios.
        p.lfo(2, LfoShape::Triangle, 5.0, paramIndex(0, OpParam::Detune), 0.2);
        p.aim(2, 1, paramIndex(1, OpParam::Detune), 0.2).aim(2, 2, paramIndex(2, OpParam::Detune), 0.2);

        p.fx(0, FxType::Delay, {{FxParam::DelayDivisionL, 15.0}, {FxParam::DelayLink, 1.0}, // 1/8D
                                {FxParam::DelayFeedback, 0.3}, {FxParam::DelayTone, -0.3}, {FxParam::DelayMix, 0.2}});
        p.fx(1, FxType::Reverb, {{FxParam::ReverbType, static_cast<double>(ReverbType::Plate)},
                                 {FxParam::ReverbSize, 0.4}, {FxParam::ReverbDecay, normFromTime(1.4)},
                                 {FxParam::ReverbMix, 0.15}});
        presets.push_back({"LD Folded Lead",
                           p.changes,
                           {{0.55f, 0.55f}, {0.25f, 0.1f}, {0.85f, 0.55f}, {0.55f, 0.1f}, {0.85f, 0.1f},
                            {0.08f, 0.55f}, {0.35f, 1.0f}, {0.65f, 1.0f}, {0.25f, 0.55f}}});
    }
    // BA Fuzz Bass
    // A fuzz bass: an FM pair distorted by a shaper on its way into a filter. B growls into A, A goes through shaper
    // 1 (the asymmetric curve, hard), and the shaper feeds a 24 dB low-pass that plucks open on each note; the
    // shaper's output also comes round onto B, dirtying the growl further. Under it a
    // clean sine an octave down, straight out. Then a compressor and a low cut.
    {
        Patch p;
        p.op(0, 1, 0.0, 0.002, 1.0, 1.0, 0.12);
        p.op(1, 1, 0.0, 0.002, 0.8, 0.6, 0.12);
        p.op(2, 0, 0.45, 0.002, 1.0, 1.0, 0.12);
        p.mod(1, 0, 0.3);
        p.shaper(0, ClipMode::Asymmetric, 20.0).set(shaperSendIndex(0, 0), 1.0).set(shaperModIndex(0, 1), 0.12);
        p.set(shaperFilterIndex(0, 0), 0.8);
        p.filter(0, FilterType::LowPass, 420.0, 0.35).filterSet(0, FilterParam::Slope, 3.0);
        p.filterSet(0, FilterParam::KeyTrack, 0.4).filterSet(0, FilterParam::Drive, 0.2);
        p.env(0, 0.0, 0.25, 0.0, 0.1, paramIndex(0, FilterParam::Cutoff), 0.3);

        p.fx(0, FxType::Compressor, {{FxParam::CompThreshold, -18.0}, {FxParam::CompRatio, 4.0},
                                     {FxParam::CompAttack, normFromTime(0.005)},
                                     {FxParam::CompRelease, normFromTime(0.12)}, {FxParam::CompMakeup, 3.0}});
        p.fx(1, FxType::ParamEq, {{FxParam::PeqType1, static_cast<double>(PeqType::LowCut)},
                                  {FxParam::PeqFreq1, normFromCutoff(30.0)}});
        presets.push_back({"BA Fuzz Bass",
                           p.changes,
                           {{0.45f, 0.55f}, {0.62f, 0.1f}, {0.85f, 0.55f}, {0.85f, 0.1f}, {0.2f, 0.1f},
                            {0.35f, 0.1f}, {0.5f, 1.0f}, {0.75f, 1.0f}, {0.15f, 1.0f}}});
    }
    // PL Comb String
    // A plucked string from a comb filter, like a koto: a short burst of noise (A), its sides apart, strikes filter 1,
    // a comb that rings at the note (its cutoff on C4, its Key Track at 100 %, its resonance all the way up), and the
    // ringing goes on through filter 2, a gentle 6 dB low-pass that follows the keys halfway, for a rounder string. A
    // half sine at the note (B) gives it a body. The mod wheel damps the string, shortening its ring. Then a widener
    // and a hall.
    {
        Patch p;
        p.op(0, 1, 0.0, 0.0, 0.2, 0.0, 0.06).opSet(0, OpParam::Wave, static_cast<double>(OpWave::Noise));
        p.opSet(0, OpParam::NoiseSlope, 0.4).opSet(0, OpParam::UnisonSpread, 0.6);
        p.op(1, 1, 0.1, 0.0, 0.8, 0.0, 0.3).opSet(1, OpParam::Wave, static_cast<double>(OpWave::HalfSine));
        p.filter(0, FilterType::Comb, 261.63, 1.0).filterSet(0, FilterParam::KeyTrack, 1.0);
        p.filterSet(0, FilterParam::Out, 0.0).set(sendIndex(0, 0), 1.0);
        p.filter(1, FilterType::LowPass, 2500.0, 0.0).filterSet(1, FilterParam::Slope, 0.0); // 6 dB
        p.filterSet(1, FilterParam::KeyTrack, 0.5).set(filterLinkIndex(0, 1), 1.0);
        p.aim(kWheelSource, 0, paramIndex(0, FilterParam::Resonance), -0.4);
        p.set(paramIndex(GlobalParam::Volume), -7.0); // a struck comb puts out less than a sounding operator

        p.fx(0, FxType::Widener, {{FxParam::WidenerSize, 0.35}, {FxParam::WidenerHighPass, normFromCutoff(250.0)},
                                  {FxParam::WidenerMix, 0.4}});
        p.fx(1, FxType::Reverb, {{FxParam::ReverbSize, 0.7}, {FxParam::ReverbDecay, normFromTime(2.2)},
                                 {FxParam::ReverbMix, 0.2}});
        // The noise over the comb, the string's way on to the low-pass, the body to the right.
        presets.push_back({"PL Comb String",
                           p.changes,
                           {{0.3f, 0.45f}, {0.8f, 0.55f}, {0.5f, 0.1f}, {0.8f, 0.1f}, {0.2f, 0.55f}, {0.8f, 0.55f},
                            {0.3f, 1.0f}, {0.6f, 1.0f}}});
    }
    // PL Chip Pluck
    // A chip-tune pluck in the classic FM sound chips' waves: an alternating sine (A), its decay falling fast and then
    // lingering, moved by a half sine (B) with feedback whose bite is gone in a quarter second, and an abs sine (C),
    // which sounds an octave up, for the chiff. The note source eases the FM off from C3 up, most in the top octaves
    // (its curve), as those chips' key scaling did, so high notes stay sweet; the Bite macro turns all the FM up. Part
    // of A goes through shaper 1's hard clipper for grit, and out beside it. Then the DJ EQ thins the lows and lifts the
    // highs, a slapback delay a different length on each side, and a cut above 9 kHz. B stands above A and C beside
    // it, so no route runs behind another node.
    {
        Patch p;
        p.op(0, 1, 0.6, 0.0, 1.5, 0.0, 0.3).opSet(0, OpParam::Wave, static_cast<double>(OpWave::AltSine));
        p.opSet(0, OpParam::DecayCurve, 0.7);
        p.op(1, 2, 0.0, 0.0, 0.25, 0.0, 0.2).opSet(1, OpParam::Wave, static_cast<double>(OpWave::HalfSine));
        p.mod(1, 0, 0.45).mod(1, 1, 0.2);
        p.op(2, 1, 0.2, 0.0, 0.35, 0.0, 0.2).opSet(2, OpParam::Wave, static_cast<double>(OpWave::AbsSine));
        p.shaper(0, ClipMode::Hard, 12.0).set(shaperSendIndex(0, 0), 0.6).shaperSet(0, ShaperParam::Out, 0.25);
        // Key scaling: from C3 (48) to C7 (96), the FM falling by up to 30 %, a fifth of that by the middle.
        p.aim(kNoteSource, 0, matrixIndex(1, 0), -0.3);
        p.set(paramIndex(NoteParam::Low), 48.0).set(paramIndex(NoteParam::High), 96.0);
        p.shape(paramIndex(NoteParam::Curve), {{0.0, -1.0, 0.0}, {0.5, -0.6, 0.0}, {1.0, 1.0, 0.0}});
        p.aim(kMaxSources, 0, paramIndex(GlobalParam::FmAmount), 0.25); // Bite: up to 150 % FM

        p.fx(0, FxType::Eq, {{FxParam::EqLow, -0.4}, {FxParam::EqHigh, 0.3}});
        p.fx(1, FxType::Delay, {{FxParam::DelaySync, 0.0}, {FxParam::DelayTimeL, normFromTime(0.11)},
                                {FxParam::DelayTimeR, normFromTime(0.17)}, {FxParam::DelayFeedback, 0.2},
                                {FxParam::DelayMix, 0.2}});
        p.fx(2, FxType::ParamEq, {{FxParam::PeqType1, static_cast<double>(PeqType::HighCut)},
                                  {FxParam::PeqFreq1, normFromCutoff(9000.0)}});
        presets.push_back({"PL Chip Pluck", p.changes, {{0.5f, 0.55f}, {0.5f, 0.1f}, {0.8f, 0.55f}}, {"Bite"}});
    }
    // SQ Step Filter
    // A step sequence drawn as an LFO shape: an organ wave (A) and a 25 % pulse an octave down (B) through a resonant
    // 18 dB low-pass, which a freeform LFO opens on every eighth note of the bar, each accent its own height and falling
    // away after it. The LFO runs on one clock, so chords and later notes stay in step. The Cutoff macro opens the
    // filter further, and the Accent macro deepens the pattern (it turns up the LFO's depth). The filter goes on into
    // shaper 1's cubic curve, which warms it, and out from there. Then a stereo phaser synced to half notes,
    // three-band compression with its bands linked, and a room.
    {
        Patch p;
        p.op(0, 1, 0.0, 0.003, 1.0, 1.0, 0.15).opSet(0, OpParam::Wave, static_cast<double>(OpWave::Organ));
        p.op(1, 0, 0.0, 0.003, 1.0, 1.0, 0.15).opSet(1, OpParam::Wave, static_cast<double>(OpWave::Pulse));
        p.filter(0, FilterType::LowPass, 280.0, 0.62).filterSet(0, FilterParam::Slope, 2.0); // 18 dB
        p.filterSet(0, FilterParam::KeyTrack, 0.4).filterSet(0, FilterParam::Drive, 0.25);
        p.filterSet(0, FilterParam::Out, 0.0).set(sendIndex(0, 0), 0.8).set(sendIndex(1, 0), 0.6);
        p.shaper(0, ClipMode::Cubic, 8.0).shaperSet(0, ShaperParam::Mix, 0.7).shaperSet(0, ShaperParam::Out, 1.0);
        p.set(filterShaperIndex(0, 0), 1.0);
        // The pattern, a bar long (division 7 is 1/1): on each eighth note a jump up to its accent, then a fall that
        // drops fast and settles (the negative curve).
        const double accents[] = {1.0, 0.25, 0.55, 0.25, 0.85, 0.25, 0.6, 0.4};
        std::vector<std::array<double, 3>> steps;
        for (int k = 0; k < 8; ++k) {
            steps.push_back({k / 8.0, 2.0 * accents[k] - 1.0, -0.4});
            steps.push_back({(k + 1) / 8.0 - 0.004, -1.0, 0.0});
        }
        p.lfo(0, LfoShape::Custom, 1.0, paramIndex(0, FilterParam::Cutoff), 0.45).shape(shapeCountIndex(0), steps);
        p.sourceSet(0, SourceParam::Sync, 1.0).sourceSet(0, SourceParam::Division, 7.0);
        p.sourceSet(0, SourceParam::Free, 1.0).sourceSet(0, SourceParam::Unipolar, 1.0);
        p.aim(kMaxSources, 0, paramIndex(0, FilterParam::Cutoff), 0.3); // Cutoff
        p.aim(kMaxSources + 1, 0, slotAmountIndex(0, 0), 0.15);        // Accent: the LFO's depth up to 75 %

        p.fx(0, FxType::Phaser, {{FxParam::PhaserSync, 1.0}, {FxParam::PhaserDivision, 10.0}, // 1/2
                                 {FxParam::PhaserStereo, 1.0}, {FxParam::PhaserDepth, 0.5},
                                 {FxParam::PhaserFeedback, 0.3}, {FxParam::PhaserMix, 0.6}});
        p.fx(1, FxType::MultiComp, {{FxParam::McLink, 1.0}, {FxParam::McThreshold1, -20.0}, {FxParam::McRatio1, 3.0},
                                    {FxParam::McGain1, 2.0}, {FxParam::McAttack, normFromTime(0.01)},
                                    {FxParam::McRelease, normFromTime(0.12)}});
        p.fx(2, FxType::Reverb, {{FxParam::ReverbType, static_cast<double>(ReverbType::Room)},
                                 {FxParam::ReverbSize, 0.35}, {FxParam::ReverbDecay, normFromTime(0.9)},
                                 {FxParam::ReverbMix, 0.12}});
        // Both operators over the filter, the shaper after it.
        presets.push_back({"SQ Step Filter",
                           p.changes,
                           {{0.3f, 0.45f}, {0.6f, 0.45f}, {0.5f, 0.1f}, {0.8f, 0.1f}, {0.2f, 0.55f}, {0.8f, 0.55f},
                            {0.45f, 1.0f}, {0.65f, 1.0f}, {0.8f, 1.0f}},
                           {"Cutoff", "Accent"}});
    }
    // SQ Glass Steps
    // Glass in sixteenths: a custom wave (A) drawn from 64 partials, the Fibonacci ones (1, 2, 3, 5 and on to 55) each
    // 3 dB below the one before, a glassy spectrum reaching high up, through a wide notch that follows the keys
    // (filter 1) and jumps to a new place every sixteenth note at random, taking out a different part of it each step.
    // A sine an octave down (B) holds the root. Then echoes in sixteenths on the left and eighths on the right, and a
    // plate.
    {
        Patch p;
        p.op(0, 1, 0.0, 0.0, 1.5, 0.35, 0.6).opSet(0, OpParam::Wave, static_cast<double>(OpWave::Custom));
        p.set(partialCountIndex(0), 64.0);
        const int fibonacci[] = {1, 2, 3, 5, 8, 13, 21, 34, 55};
        for (int k = 0; k < 9; ++k) {
            p.set(partialLevelIndex(0, fibonacci[k] - 1), std::pow(10.0, -3.0 * k / 20.0));
            p.set(partialPhaseIndex(0, fibonacci[k] - 1), k % 2 ? 0.25 : 0.0);
        }
        p.filter(0, FilterType::Notch, 2000.0, 0.15).filterSet(0, FilterParam::Slope, 3.0); // 24 dB: deep and wide
        p.filterSet(0, FilterParam::KeyTrack, 1.0).set(sendIndex(0, 0), 1.0);
        p.op(1, 0, 0.25, 0.0, 1.5, 0.5, 0.6);
        // The jumps: a random LFO stepping in sixteenths (division 19), on one clock, gliding only a little into each.
        p.lfo(0, LfoShape::Random, 1.0, paramIndex(0, FilterParam::Cutoff), 0.3);
        p.sourceSet(0, SourceParam::Sync, 1.0).sourceSet(0, SourceParam::Division, 19.0);
        p.sourceSet(0, SourceParam::Free, 1.0).sourceSet(0, SourceParam::RandomSmooth, 0.1);
        p.sourceSet(0, SourceParam::RandomChaos, 1.0);

        p.fx(0, FxType::Delay, {{FxParam::DelayDivisionL, 19.0}, {FxParam::DelayDivisionR, 16.0}, // 1/16, 1/8
                                {FxParam::DelayFeedback, 0.35}, {FxParam::DelayTone, 0.2}, {FxParam::DelayWidth, 0.8},
                                {FxParam::DelayMix, 0.25}});
        p.fx(1, FxType::Reverb, {{FxParam::ReverbType, static_cast<double>(ReverbType::Plate)},
                                 {FxParam::ReverbDecay, normFromTime(2.0)}, {FxParam::ReverbMix, 0.2}});
        presets.push_back({"SQ Glass Steps",
                           p.changes,
                           {{0.35f, 0.45f}, {0.75f, 0.55f}, {0.5f, 0.1f}, {0.8f, 0.1f}, {0.2f, 0.55f}, {0.8f, 0.55f},
                            {0.35f, 1.0f}, {0.65f, 1.0f}}});
    }
    // LD Vowel Lead
    // A singing lead: a vowel wave (A), brightened by a sine (B) whose push holds for a moment at each note before it
    // settles, and a breath of noise (C) through a band-pass that follows the keys (filter 1), which roughens A by
    // modulating it rather than going out. Mono and legato, gliding into every note (Glide Always), with the bend a
    // whole octave. A bipolar envelope scoops a phrase's first note up into pitch, a little past it and back, and the
    // mod wheel brings in vibrato (it turns up the LFO's depth, which starts at nothing). Then a high-pass that follows
    // the melody, the frequency shifter detuning the two sides a few hertz apart, eighth-note echoes and a plate.
    {
        Patch p;
        p.set(paramIndex(GlobalParam::Mono), 1.0).set(paramIndex(GlobalParam::Legato), 1.0);
        p.set(paramIndex(GlobalParam::Glide), normFromTime(0.08));
        p.set(paramIndex(GlobalParam::GlideAlways), 1.0).set(paramIndex(GlobalParam::BendRange), 12.0);
        p.op(0, 1, 0.55, 0.01, 1.0, 1.0, 0.2).opSet(0, OpParam::Wave, static_cast<double>(OpWave::Vowel));
        p.op(1, 1, 0.0, 0.01, 0.4, 0.4, 0.2).opSet(1, OpParam::Hold, normFromTime(0.06));
        p.mod(1, 0, 0.22);
        p.op(2, 1, 0.0, 0.02, 1.0, 1.0, 0.2).opSet(2, OpParam::Wave, static_cast<double>(OpWave::Noise));
        p.opSet(2, OpParam::NoiseSlope, 0.5);
        p.filter(0, FilterType::BandPass, 2000.0, 0.3).filterSet(0, FilterParam::KeyTrack, 1.0);
        p.filterSet(0, FilterParam::Out, 0.0).set(sendIndex(2, 0), 1.0).set(filterModIndex(0, 0), 0.12);
        // Vibrato on the whole voice's pitch, brought in by the mod wheel: up to a fifth of a semitone either way.
        p.lfo(0, LfoShape::Sine, 5.5, paramIndex(GlobalParam::Pitch), 0.0);
        p.aim(kWheelSource, 0, slotAmountIndex(0, 0), 0.002);
        // The scoop: from 0.3 of a semitone under the note, as far over it at the top of the attack, then onto it.
        p.env(1, 0.08, 0.15, 0.5, 0.1, paramIndex(GlobalParam::Pitch), 0.006);
        p.sourceSet(1, SourceParam::EnvBipolar, 1.0);

        p.fx(0, FxType::Filter, {{FxParam::FilterType, static_cast<double>(FilterType::HighPass)},
                                 {FxParam::FilterCutoff, normFromCutoff(150.0)}, {FxParam::FilterResonance, 0.15},
                                 {FxParam::FilterKeyTrack, 0.5}});
        p.fx(1, FxType::FreqShifter, {{FxParam::ShiftRange, normFromShiftRange(10.0)}, {FxParam::ShiftAmount, 0.3},
                                      {FxParam::ShiftSpread, 1.0}, {FxParam::ShiftMix, 0.4}});
        p.fx(2, FxType::Delay, {{FxParam::DelayDivisionL, 16.0}, {FxParam::DelayLink, 1.0}, // 1/8
                                {FxParam::DelayFeedback, 0.3}, {FxParam::DelayTone, -0.3}, {FxParam::DelayMix, 0.18}});
        p.fx(3, FxType::Reverb, {{FxParam::ReverbType, static_cast<double>(ReverbType::Plate)},
                                 {FxParam::ReverbDecay, normFromTime(1.6)}, {FxParam::ReverbMix, 0.18}});
        // The carrier in the middle, its modulator above, the breath over its band-pass to the left.
        presets.push_back({"LD Vowel Lead",
                           p.changes,
                           {{0.55f, 0.55f}, {0.55f, 0.1f}, {0.2f, 0.45f}, {0.8f, 0.1f}, {0.2f, 0.55f}, {0.8f, 0.55f},
                            {0.2f, 1.0f}, {0.65f, 1.0f}}});
    }
    // FX Laser Zap
    // A laser zap: a sine (A) with feedback, the whole voice's pitch thrown down from two octaves above the note to two
    // below by a one-shot saw LFO, once per note, in a quarter of a second; a modulator at 3.5 (B) gives the start a
    // metallic edge that dies first. The Speed macro makes the zap faster (it speeds up the LFO), and the Grit macro
    // roughens it with more feedback. Then the phase disperser smears it into a chirp, a shaper with a drawn staircase
    // curve crushes it into seven levels like a bit reducer, half mixed in, a mono flanger sweeping once a bar, and
    // ping-pong echoes.
    {
        Patch p;
        p.op(0, 1, 0.6, 0.0, 0.6, 0.0, 0.3);
        p.op(1, 3, 0.0, 0.0, 0.15, 0.0, 0.1).opSet(1, OpParam::Fine, 1.0 / 6.0);
        p.mod(0, 0, 0.25).mod(1, 0, 0.3);
        p.lfo(0, LfoShape::Saw, 4.0, paramIndex(GlobalParam::Pitch), -0.5).sourceSet(0, SourceParam::OneShot, 1.0);
        p.aim(kMaxSources, 0, paramIndex(0, SourceParam::Rate), 0.15); // Speed: up to 3.6 times as fast
        p.aim(kMaxSources + 1, 0, matrixIndex(0, 0), 0.3);            // Grit

        p.fx(0, FxType::Disperser, {{FxParam::DisperserFreq, normFromCutoff(1500.0)}, {FxParam::DisperserStages, 24.0},
                                    {FxParam::DisperserPinch, 0.7}, {FxParam::DisperserMix, 0.8}});
        // Seven levels, the middle one at nothing, so silence stays silent: each a flat step, then a jump to the next.
        std::vector<std::array<double, 3>> stairs;
        for (int k = 0; k < 7; ++k) {
            const double level = -1.0 + k / 3.0;
            stairs.push_back({k / 7.0, level, 0.0});
            stairs.push_back({k == 6 ? 1.0 : (k + 1) / 7.0 - 0.002, level, 0.0});
        }
        p.fx(1, FxType::Shaper, {{FxParam::ShaperMode, static_cast<double>(ClipMode::Custom)},
                                  {FxParam::ShaperDrive, 6.0}, {FxParam::ShaperLink, 0.0}, {FxParam::ShaperMix, 0.5}});
        p.shape(clipCurveCountIndex(1), stairs);
        p.fx(2, FxType::Flanger, {{FxParam::FlangerSync, 1.0}, {FxParam::FlangerDivision, 7.0}, // a bar
                                  {FxParam::FlangerStereo, 0.0}, {FxParam::FlangerDepth, 0.6},
                                  {FxParam::FlangerFeedback, 0.5}});
        p.fx(3, FxType::Delay, {{FxParam::DelayDivisionL, 16.0}, {FxParam::DelayLink, 1.0}, // 1/8
                                {FxParam::DelayPingPong, 1.0}, {FxParam::DelayFeedback, 0.4}, {FxParam::DelayMix, 0.2}});
        presets.push_back({"FX Laser Zap", p.changes, {}, {"Speed", "Grit"}});
    }
    // FX Noise Riser
    // A riser for builds: wide white noise (A, its sides wholly apart) fading in over six seconds, slowly and then in a
    // rush (its attack curve), through an 18 dB high-pass (filter 1), and a sine (B) that climbs two octaves, both swept
    // by one envelope that climbs evenly over eight seconds: the high-pass climbs from the lows to the top, and the
    // frequency shifter's spiral turns faster and faster. Then the shifter with feedback, a low shelf keeping the lows
    // out of the way, the widener and a long hall.
    {
        Patch p;
        p.op(0, 1, 0.0, 6.0, 1.0, 1.0, 1.5).opSet(0, OpParam::Wave, static_cast<double>(OpWave::Noise));
        p.opSet(0, OpParam::AttackCurve, -0.6).opSet(0, OpParam::UnisonSpread, 1.0);
        p.filter(0, FilterType::HighPass, 150.0, 0.35).filterSet(0, FilterParam::Slope, 2.0); // 18 dB
        p.filterSet(0, FilterParam::Out, 0.6).set(sendIndex(0, 0), 1.0);
        p.op(1, 1, 0.15, 0.6, 1.0, 1.0, 1.5);
        // The rise: the cutoff up by more than half its travel, B two octaves (24 of the Pitch knob's 96 semitones),
        // the shifter's Shift from nothing to 70 % of its 400 Hz.
        p.env(0, 8.0, 1.0, 1.0, 1.5, paramIndex(0, FilterParam::Cutoff), 0.55);
        p.sourceSet(0, SourceParam::AttackCurve, 0);
        p.aim(0, 1, paramIndex(1, OpParam::Transpose), 0.25).aim(0, 2, paramIndex(0, FxParam::ShiftAmount), 0.35);

        p.fx(0, FxType::FreqShifter, {{FxParam::ShiftRange, normFromShiftRange(400.0)}, {FxParam::ShiftSpread, 0.3},
                                      {FxParam::ShiftFeedback, 0.5}, {FxParam::ShiftMix, 0.45}});
        p.fx(1, FxType::ParamEq, {{FxParam::PeqType1, static_cast<double>(PeqType::LowShelf)},
                                  {FxParam::PeqFreq1, normFromCutoff(250.0)}, {FxParam::PeqGain1, -6.0}});
        p.fx(2, FxType::Widener, {{FxParam::WidenerSize, 0.7}, {FxParam::WidenerHighPass, normFromCutoff(300.0)},
                                  {FxParam::WidenerMix, 0.6}});
        p.fx(3, FxType::Reverb, {{FxParam::ReverbSize, 0.9}, {FxParam::ReverbDecay, normFromTime(5.0)},
                                 {FxParam::ReverbMix, 0.35}});
        presets.push_back({"FX Noise Riser", p.changes});
    }
    return presets;
}

gui::UiLayout exampleLayout(const FactoryPreset &preset) {
    gui::UiLayout layout;
    for (size_t node = 0; node < preset.nodes.size() && node < gui::kNodes; ++node) {
        const auto [x, y] = preset.nodes[node];
        if (node < kOps) {
            layout.nodeX[node] = x;
            layout.nodeY[node] = y;
        } else {
            layout.boxX[node - kOps] = x;
            layout.boxY[node - kOps] = y;
        }
    }
    for (size_t macro = 0; macro < preset.macros.size() && macro < kMacros; ++macro)
        layout.macroNames[macro] = preset.macros[macro];
    return layout;
}

} // namespace fmvibe
