#include "remotepages.h"

#include "gui/controller.h"
#include "uistrings.h"

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <string>

namespace fmvibe {

namespace {

// The knobs of an effect slot's page, for the effect it holds (and for a reverb, its algorithm).
std::vector<int> fxPageParams(int slot, FxType type, const ParamValues &values) {
    auto at = [slot](std::initializer_list<FxParam> fields) {
        std::vector<int> indices;
        for (FxParam f : fields)
            indices.push_back(paramIndex(slot, f));
        return indices;
    };
    switch (type) {
    case FxType::Shaper:
        return at({FxParam::Enabled, FxParam::ShaperMode, FxParam::ShaperDrive, FxParam::ShaperOutput, FxParam::ShaperMix,
                   FxParam::ShaperLink, FxParam::ShaperSymmetric});
    case FxType::Delay:
        return at({FxParam::Enabled, FxParam::DelayTimeL, FxParam::DelayTimeR, FxParam::DelayFeedback,
                   FxParam::DelayTone, FxParam::DelayWidth, FxParam::DelayPingPong, FxParam::DelayMix});
    case FxType::Eq:
        return at({FxParam::Enabled, FxParam::EqLow, FxParam::EqMid, FxParam::EqHigh});
    case FxType::ParamEq:
        return at({FxParam::Enabled, FxParam::PeqFreq1, FxParam::PeqGain1, FxParam::PeqQ1, FxParam::PeqFreq2,
                   FxParam::PeqGain2, FxParam::PeqQ2});
    case FxType::Filter:
        return at({FxParam::Enabled, FxParam::FilterType, FxParam::FilterSlope, FxParam::FilterCutoff,
                   FxParam::FilterResonance, FxParam::FilterKeyTrack, FxParam::FilterDrive, FxParam::FilterMix});
    case FxType::Compressor:
        return at({FxParam::Enabled, FxParam::CompThreshold, FxParam::CompRatio, FxParam::CompKnee,
                   FxParam::CompAttack, FxParam::CompRelease, FxParam::CompMakeup, FxParam::CompMix});
    case FxType::MultiDyn: // the bands' thresholds; their ratios, gains and the crossovers are left to the editor
        return {mdAboveIndex(slot, 0), mdAboveIndex(slot, 1), mdAboveIndex(slot, 2),
                mdBelowIndex(slot, 0), mdBelowIndex(slot, 1), mdBelowIndex(slot, 2),
                paramIndex(slot, FxParam::MdRange), paramIndex(slot, FxParam::MdMix)};
    case FxType::Reverb: // a shimmer's interval and amount in place of the pre-delay and width
        if (reverbType(values[paramIndex(slot, FxParam::ReverbType)]) == ReverbType::Shimmer)
            return at({FxParam::Enabled, FxParam::ReverbType, FxParam::ReverbSize, FxParam::ReverbDecay,
                       FxParam::ReverbDamping, FxParam::ReverbShimmerPitch, FxParam::ReverbShimmer, FxParam::ReverbMix});
        return at({FxParam::Enabled, FxParam::ReverbType, FxParam::ReverbSize, FxParam::ReverbDecay,
                   FxParam::ReverbDamping, FxParam::ReverbPreDelay, FxParam::ReverbWidth, FxParam::ReverbMix});
    case FxType::Phaser:
        return at({FxParam::Enabled, FxParam::PhaserRate, FxParam::PhaserDepth, FxParam::PhaserCenter,
                   FxParam::PhaserFeedback, FxParam::PhaserStages, FxParam::PhaserMix, FxParam::PhaserStereo});
    case FxType::Flanger:
        return at({FxParam::Enabled, FxParam::FlangerRate, FxParam::FlangerDepth, FxParam::FlangerDelay,
                   FxParam::FlangerFeedback, FxParam::FlangerMix, FxParam::FlangerStereo});
    case FxType::FreqShifter:
        return at({FxParam::Enabled, FxParam::ShiftRange, FxParam::ShiftAmount, FxParam::ShiftSpread,
                   FxParam::ShiftFeedback, FxParam::ShiftMix});
    case FxType::Disperser:
        return at({FxParam::Enabled, FxParam::DisperserFreq, FxParam::DisperserStages, FxParam::DisperserPinch,
                   FxParam::DisperserMix});
    case FxType::Widener:
        return at({FxParam::Enabled, FxParam::WidenerSize, FxParam::WidenerWidth, FxParam::WidenerHighPass,
                   FxParam::WidenerMix});
    case FxType::MultiComp: // retired: a state's loads as a three-band dynamics
    case FxType::None:
    case FxType::Count:
        break;
    }
    return {};
}

} // namespace

std::vector<clap_remote_controls_page_t> buildRemotePages(const ParamValues &values, const gui::UiLayout &layout) {
    std::vector<clap_remote_controls_page_t> pages;
    clap_id nextId = 0;
    auto add = [&](const char *section, const std::string &name, const std::vector<int> &indices,
                   clap_id id = CLAP_INVALID_ID) {
        clap_remote_controls_page_t page{};
        std::snprintf(page.section_name, sizeof(page.section_name), "%s", section);
        page.page_id = id != CLAP_INVALID_ID ? id : nextId++;
        std::snprintf(page.page_name, sizeof(page.page_name), "%s", name.c_str());
        std::fill(std::begin(page.param_ids), std::end(page.param_ids), CLAP_INVALID_ID);
        for (size_t i = 0; i < indices.size() && i < CLAP_REMOTE_CONTROLS_COUNT; ++i)
            page.param_ids[i] = idOfIndex(indices[i]);
        pages.push_back(page);
    };
    auto letter = [](int op) { return std::string(1, static_cast<char>('A' + op)); };

    // The macros first, as the page a host shows to begin with. Its id is its own, past the effects' (1000 on)
    // and the sources' (2000 on).
    std::vector<int> macros;
    for (int macro = 0; macro < kMacros; ++macro)
        macros.push_back(paramIndex(macro, MacroParam::Value));
    const char *const mix = tr(Str::PageMix);
    add(mix, tr(Str::PageMacros), macros, 3000);
    add(mix, tr(Str::PageGlobal),
        {paramIndex(GlobalParam::Volume), paramIndex(GlobalParam::FmAmount), paramIndex(GlobalParam::Pitch),
         paramIndex(GlobalParam::Pan)});
    add(mix, tr(Str::PagePlay),
        {paramIndex(GlobalParam::Mono), paramIndex(GlobalParam::Legato), paramIndex(GlobalParam::Glide),
         paramIndex(GlobalParam::GlideAlways), paramIndex(GlobalParam::BendRange), paramIndex(GlobalParam::Voices)});
    std::vector<int> outputs;
    for (int op = 0; op < kOps; ++op)
        outputs.push_back(paramIndex(op, OpParam::Out));
    for (int shaper = 0; shaper < kShapers; ++shaper)
        outputs.push_back(paramIndex(shaper, ShaperParam::Out));
    add(mix, tr(Str::PageOutputs), outputs);
    // Effect pages keep their slot's id whatever else changes.
    for (int slot : fxOrder(values[paramIndex(GlobalParam::FxOrder)])) {
        const FxType type = fxType(values[paramIndex(slot, FxParam::Type)]);
        if (type != FxType::None)
            add(tr(Str::ModuleFx), std::to_string(slot + 1) + " " + fxName(type), fxPageParams(slot, type, values),
                static_cast<clap_id>(1000 + slot));
    }

    for (int op = 0; op < kOps; ++op) {
        const bool fixed = values[paramIndex(op, OpParam::Fixed)] >= 0.5; // Freq and Mult in Ratio's and Fine's places
        add(tr(Str::ModuleOperators), trf(Str::ParamOp, {letter(op)}),
            {paramIndex(op, OpParam::Wave), paramIndex(op, fixed ? OpParam::FixedFreq : OpParam::Coarse),
             paramIndex(op, fixed ? OpParam::FixedMult : OpParam::Fine),
             paramIndex(op, OpParam::Detune), paramIndex(op, OpParam::Attack), paramIndex(op, OpParam::Decay),
             paramIndex(op, OpParam::Sustain), paramIndex(op, OpParam::Release)});
    }

    for (int filter = 0; filter < kFilters; ++filter) { // its own fields; what it feeds goes on the Matrix pages
        std::vector<int> fields;
        for (int f = 0; f < static_cast<int>(FilterParam::Shapers); ++f)
            fields.push_back(paramIndex(filter, FilterParam::Type) + f);
        add(tr(Str::ModuleFilters), trf(Str::ParamFilter, {std::to_string(filter + 1)}), fields);
    }
    std::vector<int> links;
    for (int from = 0; from < kFilters; ++from) // each into each, itself among them
        for (int to = 0; to < kFilters; ++to)
            links.push_back(filterLinkIndex(from, to));
    add(tr(Str::ModuleFilters), tr(Str::PageFilterLinks), links);
    // A shaper's page: its switch, curve and knobs, then what it feeds into the filters.
    for (int shaper = 0; shaper < kShapers; ++shaper) {
        std::vector<int> fields;
        for (int f = 0; f < static_cast<int>(ShaperParam::Mods); ++f)
            fields.push_back(paramIndex(shaper, ShaperParam::Enabled) + f);
        add(tr(Str::ModuleShapers), trf(Str::ParamShaper, {std::to_string(shaper + 1)}), fields);
    }

    // What each operator modulates, what goes into each filter and shaper, and what each filter and shaper
    // modulates.
    for (int from = 0; from < kOps; ++from) {
        std::vector<int> row;
        for (int to = 0; to < kOps; ++to)
            row.push_back(matrixIndex(from, to));
        add(tr(Str::ModuleMatrix), trf(Str::PageModulates, {letter(from)}), row);
    }
    for (int filter = 0; filter < kFilters; ++filter) {
        std::vector<int> row;
        for (int op = 0; op < kOps; ++op)
            row.push_back(sendIndex(op, filter));
        add(tr(Str::ModuleMatrix), trf(Str::PageInto, {"F" + std::to_string(filter + 1)}), row);
    }
    for (int filter = 0; filter < kFilters; ++filter) {
        std::vector<int> row;
        for (int to = 0; to < kOps; ++to)
            row.push_back(filterModIndex(filter, to));
        add(tr(Str::ModuleMatrix), trf(Str::PageModulates, {"F" + std::to_string(filter + 1)}), row);
    }
    for (int shaper = 0; shaper < kShapers; ++shaper) {
        std::vector<int> into, mods;
        for (int op = 0; op < kOps; ++op) {
            into.push_back(shaperSendIndex(op, shaper));
            mods.push_back(shaperModIndex(shaper, op));
        }
        for (int filter = 0; filter < kFilters; ++filter) // and from the filters
            into.push_back(filterShaperIndex(filter, shaper));
        add(tr(Str::ModuleMatrix), trf(Str::PageInto, {"S" + std::to_string(shaper + 1)}), into);
        add(tr(Str::ModuleMatrix), trf(Str::PageModulates, {"S" + std::to_string(shaper + 1)}), mods);
    }
    // The shapers into each other and themselves, which don't fit on their pages above.
    std::vector<int> shaperLoops;
    for (int from = 0; from < kShapers; ++from)
        for (int to = 0; to < kShapers; ++to)
            shaperLoops.push_back(shaperShaperIndex(from, to));
    add(tr(Str::ModuleMatrix), tr(Str::PageShaperLoops), shaperLoops);

    // Sources with their slot depths; the targets themselves are set in the editor.
    // Pages keep their place's id, like the effects'.
    auto get = [&values](int i) { return values[i]; };
    for (int source : gui::shownSources(layout, get)) {
        auto at = [source](SourceParam f) { return paramIndex(source, f); };
        const SourceType type = sourceType(values[at(SourceParam::Type)]);
        std::vector<int> fields =
            type == SourceType::Lfo ? std::vector<int>{at(SourceParam::Shape), at(SourceParam::Rate),
                                                       at(SourceParam::Sync), at(SourceParam::Division)}
            : type == SourceType::Env
                ? std::vector<int>{at(SourceParam::Attack), at(SourceParam::Decay), at(SourceParam::Sustain),
                                   at(SourceParam::Release)}
            : type == SourceType::Midi ? std::vector<int>{at(SourceParam::MidiInput), at(SourceParam::CcNumber),
                                                         at(SourceParam::Slew)}
                                       : std::vector<int>{};
        for (int slot = 0; slot < kSlots && fields.size() < CLAP_REMOTE_CONTROLS_COUNT; ++slot)
            fields.push_back(slotAmountIndex(source, slot));
        add(tr(Str::ModuleModulation), gui::sourceName(layout, get, source), fields,
            static_cast<clap_id>(2000 + source));
    }
    return pages;
}

} // namespace fmvibe
