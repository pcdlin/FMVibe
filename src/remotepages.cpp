#include "remotepages.h"

#include "gui/controller.h"
#include "uistrings.h"

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <string>

namespace fmvibe {

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

    for (int op = 0; op < kOps; ++op) {
        // Fixed, Freq and Mult take the places of Ratio and Pitch, which it ignores.
        const bool fixed = values[paramIndex(op, OpParam::Fixed)] >= 0.5;
        add(tr(Str::ModuleOperators), trf(Str::ParamOp, {letter(op)}),
            {paramIndex(op, OpParam::Wave), paramIndex(op, fixed ? OpParam::FixedFreq : OpParam::Ratio),
             paramIndex(op, fixed ? OpParam::FixedMult : OpParam::Transpose),
             paramIndex(op, OpParam::Detune), paramIndex(op, OpParam::Attack), paramIndex(op, OpParam::Decay),
             paramIndex(op, OpParam::Sustain), paramIndex(op, OpParam::Release)});
    }

    // A filter's own fields, as its page in the editor has them: Mix where Out was, which the routing graph's line to OUT
    // sets. What it feeds goes on the Matrix pages.
    for (int filter = 0; filter < kFilters; ++filter) {
        std::vector<int> fields;
        for (FilterParam f : {FilterParam::Type, FilterParam::Cutoff, FilterParam::Resonance, FilterParam::KeyTrack,
                              FilterParam::Drive, FilterParam::Mix, FilterParam::Enabled, FilterParam::Slope})
            fields.push_back(paramIndex(filter, f));
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
    // Each source its slots' depths, in the editor's order; its own knobs and the targets are set in the editor, and
    // the effects have no pages (hosts aren't given their params: see hostParamCount()). Pages keep their place's id.
    auto get = [&values](int i) { return values[i]; };
    for (int source : gui::shownSources(layout, get)) {
        std::vector<int> depths;
        for (int slot = 0; slot < kSlots && depths.size() < CLAP_REMOTE_CONTROLS_COUNT; ++slot)
            depths.push_back(slotAmountIndex(source, slot));
        add(tr(Str::ModuleModulation), gui::sourceName(layout, get, source), depths,
            static_cast<clap_id>(2000 + source));
    }
    return pages;
}

} // namespace fmvibe
