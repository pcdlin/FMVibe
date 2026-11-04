#pragma once

#include <initializer_list>
#include <string>
#include <string_view>

namespace fmvibe {

// Every piece of text the plugin shows, by name; uistrings.inc holds their wording.
// The VST3 SDK's headers have an STR macro of their own, kept around this one.
#pragma push_macro("STR")
#undef STR
enum class Str {
#define STR(id, text) id,
#include "uistrings.inc"
#undef STR
    Count
};
#pragma pop_macro("STR")

// The wording of `id`.
const char *tr(Str id);

// The wording of `id` with each {1}, {2}, ... in it replaced by that argument, so a wording can put them in any
// order: trf(Str::MsgRouteExists, {"operator A", "OUT"}).
std::string trf(Str id, std::initializer_list<std::string_view> args);

}
