#include "uistrings.h"

#include <iterator>

namespace fmvibe {

namespace {

constexpr const char *kWordings[] = {
#define STR(id, text) text,
#include "uistrings.inc"
#undef STR
};
static_assert(std::size(kWordings) == static_cast<size_t>(Str::Count));

} // namespace

const char *tr(Str id) {
    return kWordings[static_cast<int>(id)];
}

std::string trf(Str id, std::initializer_list<std::string_view> args) {
    std::string out;
    for (const char *c = tr(id); *c; ++c) {
        if (c[0] == '{' && c[1] >= '1' && c[1] <= '9' && c[2] == '}') {
            const size_t arg = static_cast<size_t>(c[1] - '1');
            if (arg < args.size())
                out += args.begin()[arg];
            c += 2;
        } else {
            out += *c;
        }
    }
    return out;
}

}
