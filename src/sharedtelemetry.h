#pragma once

#include "analyzer.h"
#include "params.h"
#include "synth.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace fmvibe {

// Telemetry handed from the audio thread to the editor. Values may come from
// different blocks, which is harmless for drawing.
struct SharedTelemetry {
    std::array<std::atomic<float>, kModulatedCount> modOffset{};
    std::array<std::atomic<float>, kAllSources * kSlots> slotOffset{};
    std::array<std::atomic<float>, kMaxSources> sources{};
    std::array<std::atomic<float>, kMacros> macroOffset{};
    std::array<std::atomic<float>, kMaxSources> lfoPhases{};
    std::array<std::atomic<float>, kMaxSources> lfoRates{};
    std::array<std::atomic<float>, kMaxSources> lfoFrom{};
    std::array<std::atomic<float>, kMaxSources> lfoTo{};
    std::array<std::atomic<float>, kOps> opLevels{};
    std::array<std::atomic<float>, kOps> opEnvLevels{};
    std::array<std::atomic<float>, kOps> opEnvPos{};
    std::array<std::atomic<float>, kOps> opTablePos{};
    std::array<std::atomic<float>, kOps> opSamplePos{};
    std::array<std::atomic<float>, kMaxSources> envPos{};
    std::atomic<float> notePitch{0.0f};
    std::atomic<float> noteVelocity{0.0f};
    std::atomic<float> bend{0.0f};
    std::atomic<float> bendSet{0.0f};
    std::atomic<float> fxPitch{60.0f};
    std::array<std::atomic<float>, kFxSlots * kMcBands> fxReduction{};
    std::array<std::atomic<float>, kFxSlots * kMcBands> fxBandLevel{};
    std::array<std::atomic<float>, kFxSlots * kMcBands> fxBandGain{};
    std::array<std::atomic<float>, kFxSlots * kSpectrumBands> fxSpectrum{};
    std::array<std::atomic<float>, kFilters * kSpectrumBands> filterSpectrum{};
    std::array<std::atomic<float>, kFxSlots> fxPhases{};
    std::array<std::atomic<float>, kFxPorts> fxPortLevels{};
    std::array<std::atomic<float>, 2> outPeak{};
    std::atomic<float> clipHold{0.0f};
    std::atomic<int64_t> stamp{0}; // steady_clock ticks when stored, for Telemetry::age
    std::atomic<int> voices{0};
    std::atomic<bool> fxTail{false};
    std::atomic<uint32_t> fxTailLfos{0};
    std::atomic<float> tempo{120.0f};
    std::array<std::atomic<uint32_t>, 4> keys{};
    std::array<std::atomic<float>, kScopeBins> scope{};
    std::atomic<int> scopeBins{0};
    std::atomic<float> scopeCursor{-1.0f};
    std::array<std::atomic<float>, kOutScopeBins> outScope{};

    void store(const Telemetry &t) {
        for (size_t i = 0; i < keys.size(); ++i)
            keys[i].store(t.keys[i], std::memory_order_relaxed);
        for (int i = 0; i < kModulatedCount; ++i)
            modOffset[i].store(t.modOffset[i], std::memory_order_relaxed);
        for (int i = 0; i < kAllSources * kSlots; ++i)
            slotOffset[i].store(t.slotOffset[i], std::memory_order_relaxed);
        for (int i = 0; i < kMacros; ++i)
            macroOffset[i].store(t.macroOffset[i], std::memory_order_relaxed);
        for (int i = 0; i < kMaxSources; ++i) {
            sources[i].store(t.sources[i], std::memory_order_relaxed);
            lfoPhases[i].store(t.lfoPhases[i], std::memory_order_relaxed);
            lfoRates[i].store(t.lfoRates[i], std::memory_order_relaxed);
            lfoFrom[i].store(t.lfoFrom[i], std::memory_order_relaxed);
            lfoTo[i].store(t.lfoTo[i], std::memory_order_relaxed);
            envPos[i].store(t.envPos[i], std::memory_order_relaxed);
        }
        for (int i = 0; i < kOps; ++i) {
            opLevels[i].store(t.opLevels[i], std::memory_order_relaxed);
            opEnvLevels[i].store(t.opEnvLevels[i], std::memory_order_relaxed);
            opEnvPos[i].store(t.opEnvPos[i], std::memory_order_relaxed);
            opTablePos[i].store(t.opTablePos[i], std::memory_order_relaxed);
            opSamplePos[i].store(t.opSamplePos[i], std::memory_order_relaxed);
        }
        notePitch.store(t.notePitch, std::memory_order_relaxed);
        noteVelocity.store(t.noteVelocity, std::memory_order_relaxed);
        bend.store(t.bend, std::memory_order_relaxed);
        bendSet.store(t.bendSet, std::memory_order_relaxed);
        fxPitch.store(t.fxPitch, std::memory_order_relaxed);
        for (int i = 0; i < kFxSlots * kMcBands; ++i) {
            fxReduction[i].store(t.fxReduction[i / kMcBands][i % kMcBands], std::memory_order_relaxed);
            fxBandLevel[i].store(t.fxBandLevel[i / kMcBands][i % kMcBands], std::memory_order_relaxed);
            fxBandGain[i].store(t.fxBandGain[i / kMcBands][i % kMcBands], std::memory_order_relaxed);
        }
        for (int i = 0; i < kFxSlots * kSpectrumBands; ++i)
            fxSpectrum[i].store(t.fxSpectrum[i / kSpectrumBands][i % kSpectrumBands], std::memory_order_relaxed);
        for (int i = 0; i < kFilters * kSpectrumBands; ++i)
            filterSpectrum[i].store(t.filterSpectrum[i / kSpectrumBands][i % kSpectrumBands], std::memory_order_relaxed);
        for (int i = 0; i < kFxSlots; ++i)
            fxPhases[i].store(t.fxPhases[i], std::memory_order_relaxed);
        for (int i = 0; i < kFxPorts; ++i)
            fxPortLevels[i].store(t.fxPortLevels[i], std::memory_order_relaxed);
        for (int c = 0; c < 2; ++c)
            outPeak[c].store(t.outPeak[c], std::memory_order_relaxed);
        clipHold.store(t.clipHold, std::memory_order_relaxed);
        voices.store(t.voices, std::memory_order_relaxed);
        fxTail.store(t.fxTail, std::memory_order_relaxed);
        fxTailLfos.store(t.fxTailLfos, std::memory_order_relaxed);
        tempo.store(t.tempo, std::memory_order_relaxed);
        for (int i = 0; i < t.scopeBins; ++i) // only what was captured: its first scopeBins
            scope[i].store(t.scope[i], std::memory_order_relaxed);
        scopeBins.store(t.scopeBins, std::memory_order_relaxed);
        scopeCursor.store(t.scopeCursor, std::memory_order_relaxed);
        for (int i = 0; i < kOutScopeBins; ++i)
            outScope[i].store(t.outScope[i], std::memory_order_relaxed);
        stamp.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
    }

    void load(Telemetry &t) const {
        for (size_t i = 0; i < keys.size(); ++i)
            t.keys[i] = keys[i].load(std::memory_order_relaxed);
        for (int i = 0; i < kModulatedCount; ++i)
            t.modOffset[i] = modOffset[i].load(std::memory_order_relaxed);
        for (int i = 0; i < kAllSources * kSlots; ++i)
            t.slotOffset[i] = slotOffset[i].load(std::memory_order_relaxed);
        for (int i = 0; i < kMacros; ++i)
            t.macroOffset[i] = macroOffset[i].load(std::memory_order_relaxed);
        for (int i = 0; i < kMaxSources; ++i) {
            t.sources[i] = sources[i].load(std::memory_order_relaxed);
            t.lfoPhases[i] = lfoPhases[i].load(std::memory_order_relaxed);
            t.lfoRates[i] = lfoRates[i].load(std::memory_order_relaxed);
            t.lfoFrom[i] = lfoFrom[i].load(std::memory_order_relaxed);
            t.lfoTo[i] = lfoTo[i].load(std::memory_order_relaxed);
            t.envPos[i] = envPos[i].load(std::memory_order_relaxed);
        }
        for (int i = 0; i < kOps; ++i) {
            t.opLevels[i] = opLevels[i].load(std::memory_order_relaxed);
            t.opEnvLevels[i] = opEnvLevels[i].load(std::memory_order_relaxed);
            t.opEnvPos[i] = opEnvPos[i].load(std::memory_order_relaxed);
            t.opTablePos[i] = opTablePos[i].load(std::memory_order_relaxed);
            t.opSamplePos[i] = opSamplePos[i].load(std::memory_order_relaxed);
        }
        t.notePitch = notePitch.load(std::memory_order_relaxed);
        t.noteVelocity = noteVelocity.load(std::memory_order_relaxed);
        t.bend = bend.load(std::memory_order_relaxed);
        t.bendSet = bendSet.load(std::memory_order_relaxed);
        t.fxPitch = fxPitch.load(std::memory_order_relaxed);
        for (int i = 0; i < kFxSlots * kMcBands; ++i) {
            t.fxReduction[i / kMcBands][i % kMcBands] = fxReduction[i].load(std::memory_order_relaxed);
            t.fxBandLevel[i / kMcBands][i % kMcBands] = fxBandLevel[i].load(std::memory_order_relaxed);
            t.fxBandGain[i / kMcBands][i % kMcBands] = fxBandGain[i].load(std::memory_order_relaxed);
        }
        for (int i = 0; i < kFxSlots * kSpectrumBands; ++i)
            t.fxSpectrum[i / kSpectrumBands][i % kSpectrumBands] = fxSpectrum[i].load(std::memory_order_relaxed);
        for (int i = 0; i < kFilters * kSpectrumBands; ++i)
            t.filterSpectrum[i / kSpectrumBands][i % kSpectrumBands] = filterSpectrum[i].load(std::memory_order_relaxed);
        for (int i = 0; i < kFxSlots; ++i)
            t.fxPhases[i] = fxPhases[i].load(std::memory_order_relaxed);
        for (int i = 0; i < kFxPorts; ++i)
            t.fxPortLevels[i] = fxPortLevels[i].load(std::memory_order_relaxed);
        for (int c = 0; c < 2; ++c)
            t.outPeak[c] = outPeak[c].load(std::memory_order_relaxed);
        t.clipHold = clipHold.load(std::memory_order_relaxed);
        t.voices = voices.load(std::memory_order_relaxed);
        t.fxTail = fxTail.load(std::memory_order_relaxed);
        t.fxTailLfos = fxTailLfos.load(std::memory_order_relaxed);
        t.tempo = tempo.load(std::memory_order_relaxed);
        t.scopeBins = std::clamp(scopeBins.load(std::memory_order_relaxed), 0, kScopeBins);
        for (int i = 0; i < t.scopeBins; ++i)
            t.scope[i] = scope[i].load(std::memory_order_relaxed);
        t.scopeCursor = scopeCursor.load(std::memory_order_relaxed);
        for (int i = 0; i < kOutScopeBins; ++i)
            t.outScope[i] = outScope[i].load(std::memory_order_relaxed);
        const auto elapsed = std::chrono::steady_clock::duration(
            std::chrono::steady_clock::now().time_since_epoch().count() - stamp.load(std::memory_order_relaxed));
        t.age = std::chrono::duration<float>(elapsed).count();
    }
};

} // namespace fmvibe
