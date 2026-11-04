// The VST3 module's entry: hosts call the exported GetPluginFactory() and make plugin instances through the factory
// it returns. Windows hosts also call InitDll() after loading the module and ExitDll() before unloading it.

#include "plugincore.h"
#include "vst3plugin.h"

#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>

#include <windows.h>

#include <cstring>

namespace {

using namespace Steinberg;

void copyWide(char16 *to, size_t capacity, const char *ascii) {
    size_t i = 0;
    for (; ascii[i] && i + 1 < capacity; ++i)
        to[i] = static_cast<char16>(ascii[i]);
    to[i] = 0;
}

void copyNarrow(char8 *to, size_t capacity, const char *text) {
    strncpy_s(to, capacity, text, _TRUNCATE);
}

// One class: FMVibe, an instrument whose component is its own controller.
class Factory final : public IPluginFactory3 {
public:
    tresult PLUGIN_API queryInterface(const TUID queried, void **obj) override {
        QUERY_INTERFACE(queried, obj, FUnknown::iid, IPluginFactory3)
        QUERY_INTERFACE(queried, obj, IPluginFactory::iid, IPluginFactory3)
        QUERY_INTERFACE(queried, obj, IPluginFactory2::iid, IPluginFactory3)
        QUERY_INTERFACE(queried, obj, IPluginFactory3::iid, IPluginFactory3)
        *obj = nullptr;
        return kNoInterface;
    }
    // One factory for the module's life.
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }

    tresult PLUGIN_API getFactoryInfo(PFactoryInfo *info) override {
        if (!info)
            return kInvalidArgument;
        *info = PFactoryInfo();
        copyNarrow(info->vendor, PFactoryInfo::kNameSize, fmvibe::kPluginVendor);
        info->flags = PFactoryInfo::kUnicode;
        return kResultOk;
    }
    int32 PLUGIN_API countClasses() override { return 1; }
    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo *info) override {
        if (index != 0 || !info)
            return kInvalidArgument;
        *info = PClassInfo();
        std::memcpy(info->cid, fmvibe::vst3::kClassId, sizeof(TUID));
        info->cardinality = PClassInfo::kManyInstances;
        copyNarrow(info->category, PClassInfo::kCategorySize, kVstAudioEffectClass);
        copyNarrow(info->name, PClassInfo::kNameSize, fmvibe::kPluginName);
        return kResultOk;
    }
    tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2 *info) override {
        if (index != 0 || !info)
            return kInvalidArgument;
        *info = PClassInfo2();
        std::memcpy(info->cid, fmvibe::vst3::kClassId, sizeof(TUID));
        info->cardinality = PClassInfo::kManyInstances;
        copyNarrow(info->category, PClassInfo::kCategorySize, kVstAudioEffectClass);
        copyNarrow(info->name, PClassInfo::kNameSize, fmvibe::kPluginName);
        info->classFlags = 0; // not distributable: the processor and controller are one
        copyNarrow(info->subCategories, PClassInfo2::kSubCategoriesSize, Vst::PlugType::kInstrumentSynth);
        copyNarrow(info->vendor, PClassInfo2::kVendorSize, fmvibe::kPluginVendor);
        copyNarrow(info->version, PClassInfo2::kVersionSize, FMVIBE_VERSION);
        copyNarrow(info->sdkVersion, PClassInfo2::kVersionSize, kVstVersionString);
        return kResultOk;
    }
    tresult PLUGIN_API getClassInfoUnicode(int32 index, PClassInfoW *info) override {
        if (index != 0 || !info)
            return kInvalidArgument;
        *info = PClassInfoW();
        std::memcpy(info->cid, fmvibe::vst3::kClassId, sizeof(TUID));
        info->cardinality = PClassInfo::kManyInstances;
        copyNarrow(info->category, PClassInfo::kCategorySize, kVstAudioEffectClass);
        copyWide(info->name, PClassInfo::kNameSize, fmvibe::kPluginName);
        info->classFlags = 0;
        copyNarrow(info->subCategories, PClassInfoW::kSubCategoriesSize, Vst::PlugType::kInstrumentSynth);
        copyWide(info->vendor, PClassInfoW::kVendorSize, fmvibe::kPluginVendor);
        copyWide(info->version, PClassInfoW::kVersionSize, FMVIBE_VERSION);
        copyWide(info->sdkVersion, PClassInfoW::kVersionSize, kVstVersionString);
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(FIDString cid, FIDString wanted, void **obj) override {
        if (!obj)
            return kInvalidArgument;
        *obj = nullptr;
        if (!cid || !wanted || std::memcmp(cid, fmvibe::vst3::kClassId, sizeof(TUID)) != 0)
            return kNoInterface;
        FUnknown *plugin = fmvibe::vst3::createPlugin();
        const tresult result = plugin->queryInterface(wanted, obj);
        plugin->release(); // the caller's reference, if it took one, keeps it
        return result;
    }
    tresult PLUGIN_API setHostContext(FUnknown *) override { return kResultOk; }
};

Factory factory;

} // namespace

extern "C" {

SMTG_EXPORT_SYMBOL IPluginFactory *PLUGIN_API GetPluginFactory() {
    return &factory;
}

SMTG_EXPORT_SYMBOL bool InitDll() {
    return true;
}

SMTG_EXPORT_SYMBOL bool ExitDll() {
    return true;
}

} // extern "C"
