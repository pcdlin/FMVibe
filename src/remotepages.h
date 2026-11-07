#pragma once

#include "params.h"

#include <clap/ext/remote-controls.h>

#include <vector>

namespace fmvibe {

namespace gui {
struct UiLayout;
}

// Pages of 8 knobs that hosts like Bitwig show on the device: the macros, the globals, the operators, the filters,
// the shapers, the matrix, and each source's slot depths in the order the editor shows them. The effects have none,
// since hosts aren't given their params.
std::vector<clap_remote_controls_page_t> buildRemotePages(const ParamValues &values, const gui::UiLayout &layout);

} // namespace fmvibe
