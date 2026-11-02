#pragma once

#include "params.h"

#include <clap/ext/remote-controls.h>

#include <vector>

namespace fmvibe {

namespace gui {
struct UiLayout;
}

// Pages of 8 knobs that hosts like Bitwig show on the device. Each effect in
// the chain gets a page, in chain order, for whatever it is; so does each
// source, in the order the editor shows them.
std::vector<clap_remote_controls_page_t> buildRemotePages(const ParamValues &values, const gui::UiLayout &layout);

} // namespace fmvibe
