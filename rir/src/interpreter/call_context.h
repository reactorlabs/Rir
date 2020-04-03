#ifndef RIR_INTERPRETER_DATA_C_H
#define RIR_INTERPRETER_DATA_C_H

#include "../config.h"
#include <assert.h>
#include <stdint.h>

#include "runtime/Code.h"
#include "runtime/DispatchTable.h"
#include "runtime/Function.h"

#include "R/Symbols.h"
#include "R/r.h"

#include "LazyEnvironment.h"
#include "instance.h"
#include "interp_incl.h"
#include "safe_force.h"

namespace rir {

struct CallContext {
    CallContext(Code* c, void* callSiteAddress, SEXP callee, size_t nargs,
                SEXP ast, R_bcstack_t* stackArgs, Immediate* names,
                SEXP callerEnv, const Assumptions& givenAssumptions,
                InterpreterInstance* ctx)
        : caller(c), callSiteAddress(callSiteAddress), suppliedArgs(nargs),
          passedArgs(nargs), stackArgs(stackArgs), names(names),
          callerEnv(callerEnv), ast(ast), callee(callee),
          givenAssumptions(givenAssumptions) {
        assert(callerEnv);
        assert(callee &&
               (TYPEOF(callee) == CLOSXP || TYPEOF(callee) == SPECIALSXP ||
                TYPEOF(callee) == BUILTINSXP));
        SLOWASSERT(callerEnv == symbol::delayedEnv ||
                   TYPEOF(callerEnv) == ENVSXP || callerEnv == R_NilValue ||
                   LazyEnvironment::check(callerEnv));
    }

    CallContext(Code* c, SEXP callee, size_t nargs, SEXP ast,
                R_bcstack_t* stackArgs, Immediate* names, SEXP callerEnv,
                const Assumptions& givenAssumptions, InterpreterInstance* ctx)
        : CallContext(c, nullptr, callee, nargs, ast, stackArgs, names,
                      callerEnv, givenAssumptions, ctx) {}

    // cppcheck-suppress uninitMemberVar
    CallContext(Code* c, void* callSiteAddress, SEXP callee, size_t nargs,
                Immediate ast, R_bcstack_t* stackArgs, Immediate* names,
                SEXP callerEnv, const Assumptions& givenAssumptions,
                InterpreterInstance* ctx)
        : CallContext(c, callSiteAddress, callee, nargs, cp_pool_at(ctx, ast),
                      stackArgs, names, callerEnv, givenAssumptions, ctx) {}

    // cppcheck-suppress uninitMemberVar
    CallContext(Code* c, SEXP callee, size_t nargs, Immediate ast,
                R_bcstack_t* stackArgs, Immediate* names, SEXP callerEnv,
                const Assumptions& givenAssumptions, InterpreterInstance* ctx)
        : CallContext(c, callee, nargs, cp_pool_at(ctx, ast), stackArgs, names,
                      callerEnv, givenAssumptions, ctx) {}

    // cppcheck-suppress uninitMemberVar
    CallContext(Code* c, void* callSiteAddress, SEXP callee, size_t nargs,
                Immediate ast, R_bcstack_t* stackArgs, SEXP callerEnv,
                const Assumptions& givenAssumptions, InterpreterInstance* ctx)
        : CallContext(c, callSiteAddress, callee, nargs, cp_pool_at(ctx, ast),
                      stackArgs, nullptr, callerEnv, givenAssumptions, ctx) {}

    // cppcheck-suppress uninitMemberVar
    CallContext(Code* c, SEXP callee, size_t nargs, Immediate ast,
                R_bcstack_t* stackArgs, SEXP callerEnv,
                const Assumptions& givenAssumptions, InterpreterInstance* ctx)
        : CallContext(c, callee, nargs, cp_pool_at(ctx, ast), stackArgs,
                      nullptr, callerEnv, givenAssumptions, ctx) {}

    const Code* caller;
    const void* callSiteAddress;
    const size_t suppliedArgs;
    size_t passedArgs;
    const R_bcstack_t* stackArgs;
    const Immediate* names;
    SEXP callerEnv;
    const SEXP ast;
    const SEXP callee;
    Assumptions givenAssumptions;
    SEXP arglist = nullptr;

    bool hasEagerCallee() const { return TYPEOF(callee) == BUILTINSXP; }
    bool hasNames() const { return names; }

    SEXP stackArg(unsigned i) const {
        assert(stackArgs && i < passedArgs);
        return ostack_at_cell(stackArgs + i);
    }

    SEXP name(unsigned i, InterpreterInstance* ctx) const {
        assert(hasNames() && i < suppliedArgs);
        return cp_pool_at(ctx, names[i]);
    }

    void safeForceArgs() const {
        for (unsigned i = 0; i < passedArgs; i++) {
            SEXP arg = stackArg(i);
            if (TYPEOF(arg) == PROMSXP) {
                safeForcePromise(arg);
            }
        }
    }
};

} // namespace rir

#endif // RIR_INTERPRETER_C_H
