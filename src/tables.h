#pragma once

// FMVibe's own wavetables (see FactoryTable), generated in code: nothing embedded, nothing to license. A patch names
// one by its number, so a generator, once released, is never changed; tests/quality.cpp checks each one's spectrum.

#include "params.h"

#include <vector>

namespace fmvibe {

// Frames a factory table has.
constexpr int kFactoryFrames = 64;

// Factory table `table`'s frames, kFactoryFrames of FrameTable::kFrameSize samples each, frame after frame, before the
// table's gain; empty for a number that isn't one.
std::vector<float> factoryTableFrames(int table);

} // namespace fmvibe
