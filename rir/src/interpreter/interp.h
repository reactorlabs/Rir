#ifndef RIR_INTERPRETER_C_H
#define RIR_INTERPRETER_C_H

#include "builtins.h"
#include "call_context.h"
#include "instance.h"

#include "compiler/parameter.h"
#include "interp_incl.h"
#include "ir/Deoptimization.h"

#include "R/BuiltinIds.h"

#include <R/r.h>

#undef length

#if defined(__GNUC__) && (!defined(NO_THREADED_CODE))
#define THREADED_CODE
#endif

namespace rir {
SEXP dispatchApply(SEXP ast, SEXP obj, SEXP actuals, SEXP selector,
                   SEXP callerEnv, InterpreterInstance* ctx);
bool isMissing(SEXP symbol, SEXP environment, Code* code, Opcode* op);

inline RCNTXT* getFunctionContext(size_t pos = 0,
                                  RCNTXT* cptr = (RCNTXT*)R_GlobalContext) {
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
    auto cptr = (RCNTXT*)R_GlobalContext;
    while (cptr->nextcontext != NULL) {
        if (cptr->callflag & CTXT_FUNCTION) {
            if (cptr->cloenv == e)
                return cptr;
        }
        cptr = cptr->nextcontext;
    }
    return nullptr;
}

inline bool RecompileHeuristic(DispatchTable* table, Function* fun,
                               unsigned factor = 1) {
    auto& flags = fun->flags;
    return (!flags.contains(Function::NotOptimizable) &&
            (flags.contains(Function::MarkOpt) ||
             (fun->deoptCount() < pir::Parameter::DEOPT_ABANDON &&
              ((fun != table->baseline() && fun->invocationCount() >= 2 &&
                fun->invocationCount() <= pir::Parameter::RIR_WARMUP) ||
               (fun->invocationCount() %
                (factor * (pir::Parameter::RIR_WARMUP))) == 0))));
}

inline bool RecompileCondition(DispatchTable* table, Function* fun,
                               const Context& context) {
    return (fun->flags.contains(Function::MarkOpt) ||
            fun == table->baseline() ||
            (context.smaller(fun->context()) && context.isImproving(fun)) ||
            fun->body()->flags.contains(Code::Reoptimise));
}

inline void DoRecompile(Function* fun, SEXP ast, SEXP callee, Context given,
                        InterpreterInstance* ctx) {
    // We have more assumptions available, let's recompile
    // More assumptions are available than this version uses. Let's
    // try compile a better matching version.
    auto flags = fun->flags;
#ifdef DEBUG_DISPATCH
    std::cout << "Optimizing for new context " << fun->invocationCount()
              << ": ";
    Rf_PrintValue(ast);
    std::cout << given << " vs " << fun->context() << "\n";
#endif
    SEXP lhs = CAR(ast);
    SEXP name = R_NilValue;
    if (TYPEOF(lhs) == SYMSXP)
        name = lhs;
    if (flags.contains(Function::MarkOpt))
        fun->flags.reset(Function::MarkOpt);
    ctx->closureOptimizer(callee, given, name);
}

inline bool matches(const CallContext& call, Function* f) {
    return call.givenContext.smaller(f->context());
}

inline Function* dispatch(const CallContext& call, DispatchTable* vt) {
    auto f = vt->dispatch(call.givenContext);
    assert(f);
    return f;
};

void inferCurrentContext(CallContext& call, size_t formalNargs,
                         InterpreterInstance* ctx);

SEXP builtinCall(CallContext& call, InterpreterInstance* ctx);
SEXP doCall(CallContext& call, InterpreterInstance* ctx);
size_t expandDotDotDotCallArgs(InterpreterInstance* ctx, size_t n,
                               Immediate* names_, SEXP env, bool explicitDots);
void deoptFramesWithContext(InterpreterInstance* ctx,
                            const CallContext* callCtxt,
                            DeoptMetadata* deoptData, SEXP sysparent,
                            size_t pos, size_t stackHeight,
                            RCNTXT* currentContext);
void recordDeoptReason(SEXP val, const DeoptReason& reason);
void jit(SEXP cls, SEXP name, InterpreterInstance* ctx);

SEXP seq_int(int n1, int n2);
bool doubleCanBeCastedToInteger(double n);
int colonInputEffects(SEXP lhs, SEXP rhs, unsigned srcIdx);
bool isColonFastcase(SEXP, SEXP);
SEXP colonCastLhs(SEXP lhs);
SEXP colonCastRhs(SEXP newLhs, SEXP rhs);

inline void forceAll(SEXP list, InterpreterInstance* ctx) {
    while (list != R_NilValue) {
        if (TYPEOF(CAR(list)) == PROMSXP)
            SETCAR(list, evaluatePromise(CAR(list), ctx));
        list = CDR(list);
    }
}

inline bool needsExpandedDots(SEXP callee) {
    return TYPEOF(callee) != SPECIALSXP ||
           // forceAndCall is fully handled in tryFastSpecialCall
           // and expects expanded dots
           callee->u.primsxp.offset == blt("forceAndCall");
}

} // namespace rir
#endif // RIR_INTERPRETER_C_H
