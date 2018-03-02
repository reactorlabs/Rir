#include "tag.h"

#include <cassert>

namespace rir {
namespace pir {

const char* TagToStr(Tag tag) {
    switch (tag) {
#define V(I)                                                                   \
    case Tag::I:                                                               \
        return #I;
        COMPILER_INSTRUCTIONS(V)
#undef V
#define V(I)                                                                   \
    case Tag::I:                                                               \
        assert(false);
        COMPILER_VALUES(V)
#undef V
    case Tag::Unused:
        assert(false);
    }
    assert(false);
    return "";
}
}
}
