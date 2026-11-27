#include "tuningedits.h"

#include <algorithm>

namespace fmvibe::gui {

namespace {

// `name` held to kMaxTuningName bytes, cut between whole UTF-8 characters.
std::string held(std::string name) {
    if (name.size() <= kMaxTuningName)
        return name;
    size_t end = kMaxTuningName;
    while (end > 0 && (static_cast<unsigned char>(name[end]) & 0xC0) == 0x80)
        --end;
    name.resize(end);
    return name;
}

// The changes and the name, one undo step: the params as one gesture, as the editor edits many at once.
void apply(Controller &c, const std::vector<std::pair<int, double>> &changes, bool mapping, const std::string &name) {
    const ParamTable &table = paramTable();
    c.beginStep();
    for (const auto &[index, v] : changes)
        c.beginEdit(index);
    for (const auto &[index, v] : changes)
        c.setValue(index, std::clamp(v, table[index].min, table[index].max));
    for (auto it = changes.rbegin(); it != changes.rend(); ++it)
        c.endEdit(it->first);
    (mapping ? c.layout().mappingName : c.layout().tuningName) = held(name);
    c.layoutChanged();
    c.endStep();
}

} // namespace

std::optional<Str> loadTuningText(Controller &c, std::string_view text, TuningFile file, const std::string &name) {
    Str why = Str::TuningUnknownFile;
    switch (file) {
    case TuningFile::Scala:
    case TuningFile::AbletonScala:
    case TuningFile::AnaMark: {
        TuningScale scale;
        const bool read = file == TuningFile::AnaMark ? parseTun(text, scale, why)
                                                      : parseScl(text, file == TuningFile::AbletonScala, scale, why);
        if (!read)
            return why;
        apply(c, scaleChanges(&scale), false, name);
        return std::nullopt;
    }
    case TuningFile::Mapping: {
        TuningMapping mapping;
        if (!parseKbm(text, mapping, why))
            return why;
        apply(c, mappingChanges(&mapping), true, name);
        return std::nullopt;
    }
    case TuningFile::Unknown:
        break;
    }
    return why;
}

void resetTuning(Controller &c, bool mapping) {
    apply(c, mapping ? mappingChanges(nullptr) : scaleChanges(nullptr), mapping, std::string());
}

bool patchHasScale(const Controller &c) {
    return c.value(paramIndex(TuningParam::Degrees)) >= 0.5;
}

bool patchHasMapping(const Controller &c) {
    return c.value(paramIndex(TuningParam::MapOn)) >= 0.5;
}

TuningStatus withPatchTuning(const Controller &c, const UiLayout &ui, TuningStatus live) {
    const bool scale = patchHasScale(c);
    if (!scale && !patchHasMapping(c))
        return live;
    if (live.source != TuningSource::Master) { // a master plays over it
        const std::string &name = scale ? ui.tuningName : ui.mappingName;
        live.source = TuningSource::Patch;
        live.scale = name.empty() ? std::string(tr(Str::SettingsMtsUnnamed)) : name;
    }
    return live;
}

} // namespace fmvibe::gui
