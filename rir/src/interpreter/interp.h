#ifndef RIR_INTERPRETER_C_H
#define RIR_INTERPRETER_C_H

#include "builtins.h"
#include "call_context.h"
#include "instance.h"

#include "interp_incl.h"

#include <R/r.h>

#undef length

#if defined(__GNUC__) && (!defined(NO_THREADED_CODE))
#define THREADED_CODE
#endif

namespace rir {

inline RCNTXT* getFunctionContext(size_t pos = 0,
                                  RCNTXT* cptr = R_GlobalContext) {
    while (cptr->nextcontext != NULL) {
        if (cptr->callflag & CTXT_FUNCTION) {
            if (pos == 0)
                return cptr;
            pos--;
        }
        cptr = cptr->nextcontext;
    }
    assert(false);
    return nullptr;
}

inline RCNTXT* findFunctionContextFor(SEXP e) {
    auto cptr = R_GlobalContext;
    while (cptr->nextcontext != NULL) {
        if (cptr->callflag & CTXT_FUNCTION && cptr->cloenv == e) {
            return cptr;
        }
        cptr = cptr->nextcontext;
    }
    return nullptr;
}
}

#endif // RIR_INTERPRETER_C_H
