#pragma once

// Who the plugin is, as both formats and the preset discovery report it. The CLAP id is stored in host projects:
// changing it makes hosts treat FMVibe as a different plugin (see "Plugin facts" in AGENTS.md).

namespace fmvibe {

inline constexpr char kPluginId[] = "io.github.pcdlin.fmvibe";
inline constexpr char kPluginName[] = "FMVibe";
inline constexpr char kPluginVendor[] = "pcdlin";
inline constexpr char kPluginDescription[] = "Six-operator FM synthesizer";

} // namespace fmvibe
