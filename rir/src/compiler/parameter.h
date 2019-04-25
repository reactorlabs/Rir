#ifndef PIR_PARAMETER_H
#define PIR_PARAMETER_H

#include "../common.h"
#include <stddef.h>

namespace rir {
namespace pir {

struct Parameter {
    static bool DEBUG_DEOPTS;
    static bool DEOPT_CHAOS;
    static bool DEOPT_CHAOS_SEED;
    static size_t MAX_INPUT_SIZE;
    static unsigned RIR_WARMUP;

    static size_t INLINER_MAX_SIZE;
    static size_t INLINER_MAX_INLINEE_SIZE;
    static size_t INLINER_INITIAL_FUEL;

    static bool RIR_STRONG_SAFE_FORCE;
    static bool RIR_CHECK_PIR_TYPES;
};
} // namespace pir
} // namespace rir

#endif
