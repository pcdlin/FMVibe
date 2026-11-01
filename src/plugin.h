#pragma once

#include <clap/clap.h>

namespace fmvibe {

extern const clap_plugin_descriptor_t kDescriptor;

// Returns the clap_plugin_t the factory hands to the host; destroy() frees it.
const clap_plugin_t *createPlugin(const clap_host_t *host);

} // namespace fmvibe
