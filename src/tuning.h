#pragma once

// A tuning the synth plays its notes in other than 12-TET (see Synth::setTuning()). The plugins hand it MTS-ESP's
// (src/mtstuning.h); the engine knows nothing of where a tuning comes from, so the browser demo and the tools play
// the patch's own tuning alone (see tuningfile.h), and a test can hand the synth a tuning of its own.

namespace fmvibe {

class Tuning {
public:
    virtual ~Tuning() = default;
    // How far `key` (0..127) on `channel` (0..15, or -1 where the channel says nothing of the tuning) is tuned away
    // from 12-TET, in semitones. Asked on the audio thread only, for each voice as its note starts and, retuning
    // continuously, every block.
    virtual double retune(int key, int channel) = 0;
    // Whether the tuning leaves `key` on `channel` unmapped: a note there isn't to sound.
    virtual bool unmapped(int key, int channel) = 0;
    // Whether it plays over a tuning the patch has of its own (an MTS-ESP master tuning the whole project), rather than
    // under it (MTS SysEx, which tunes the synth only while the patch has none). Audio thread.
    virtual bool overrides() { return false; }
};

} // namespace fmvibe
