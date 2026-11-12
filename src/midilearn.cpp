#include "midilearn.h"

#include <algorithm>
#include <cmath>

namespace fmvibe {

bool midiLearnable(int index) {
    if (index < 0 || index >= kParamCount || index == paramIndex(GlobalParam::Voices) || isRetired(index))
        return false;
    const ParamDef &def = paramTable()[index];
    if ((def.flags & CLAP_PARAM_IS_HIDDEN) || !(def.max > def.min))
        return false;
    // Not the wiring, which the editor sets by other means than a knob.
    switch (def.kind) {
    case ParamKind::ModTarget:
    case ParamKind::SlotAux:
    case ParamKind::SlotBypass:
    case ParamKind::FxOrder:
    case ParamKind::FxPosition:
    case ParamKind::FxPort:
    case ParamKind::FxType:
    case ParamKind::SourceType:
    case ParamKind::ShapeCount:
    case ParamKind::ShapePoint:
        return false;
    default:
        return true;
    }
}

double midiLearnValue(int index, double cc) {
    const ParamDef &def = paramTable()[index];
    const double value = def.min + std::clamp(std::isfinite(cc) ? cc : 0.0, 0.0, 1.0) * (def.max - def.min);
    return std::clamp(isStepped(def.kind) ? std::round(value) : value, def.min, def.max);
}

int MidiLearn::ccOf(int index) const {
    if (index < 0)
        return -1;
    for (int cc = 0; cc < kLearnableCcs; ++cc)
        if (params_[cc].load(std::memory_order_relaxed) == index)
            return cc;
    return -1;
}

void MidiLearn::arm(int index) {
    armed_.store(midiLearnable(index) ? index : -1, std::memory_order_relaxed);
    moved();
}

void MidiLearn::forget(int index) {
    if (index < 0)
        return;
    for (auto &param : params_) {
        int was = index;
        param.compare_exchange_strong(was, -1, std::memory_order_relaxed);
    }
    moved();
}

std::vector<MidiMapping> MidiLearn::mappings() const {
    std::vector<MidiMapping> list;
    for (int cc = 0; cc < kLearnableCcs; ++cc)
        if (const int index = params_[cc].load(std::memory_order_relaxed); index >= 0)
            list.push_back({cc, index});
    return list;
}

void MidiLearn::set(const std::vector<MidiMapping> &mappings) {
    std::array<int, kLearnableCcs> next;
    next.fill(-1);
    for (const MidiMapping &m : mappings) {
        if (m.cc < 0 || m.cc >= kLearnableCcs || !midiLearnable(m.index))
            continue;
        std::replace(next.begin(), next.end(), m.index, -1); // a param takes one CC: its last
        next[m.cc] = m.index;
    }
    for (int cc = 0; cc < kLearnableCcs; ++cc)
        params_[cc].store(next[cc], std::memory_order_relaxed);
    armed_.store(-1, std::memory_order_relaxed);
    moved();
}

void MidiLearn::clear() {
    for (auto &param : params_)
        param.store(-1, std::memory_order_relaxed);
    armed_.store(-1, std::memory_order_relaxed);
    moved();
}

bool MidiLearn::learn(int cc) {
    if (cc < 0 || cc >= kLearnableCcs || armed_.load(std::memory_order_relaxed) < 0)
        return false;
    const int index = armed_.exchange(-1, std::memory_order_relaxed);
    if (index < 0)
        return false;
    for (auto &param : params_) { // the CC it had before, if any, gives it up
        int was = index;
        param.compare_exchange_strong(was, -1, std::memory_order_relaxed);
    }
    params_[cc].store(index, std::memory_order_relaxed);
    moved();
    return true;
}

} // namespace fmvibe
