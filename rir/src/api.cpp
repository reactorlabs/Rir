/** Enables the use of R internals for us so that we can manipulate R structures
 * in low level.
 */

#include <cassert>

#include "api.h"

#include "ir/Compiler.h"
#include "interpreter/interp_context.h"
#include "interpreter/interp.h"
#include "ir/BC.h"

#include "analysis/Signature.h"
#include "analysis/liveness.h"
#include "analysis_framework/analysis.h"
#include "optimization/cp.h"
#include "utils/Printer.h"

#include "ir/Optimizer.h"

using namespace rir;

REXPORT SEXP rir_disassemble(SEXP what, SEXP verbose) {

    Function* f = TYPEOF(what) == CLOSXP ? isValidClosureSEXP(what) : isValidFunctionSEXP(what);

    if (f == nullptr)
        Rf_error("Not a rir compiled code");

    Rprintf("%p  [invoked %ux]\n", what, f->invocationCount);

    CodeEditor(what).print(LOGICAL(verbose)[0]);
    return R_NilValue;
}

REXPORT SEXP rir_compile(SEXP what, SEXP env = NULL) {

    // TODO make this nicer
    if (TYPEOF(what) == CLOSXP) {
        SEXP body = BODY(what);
        if (TYPEOF(body) == EXTERNALSXP)
            Rf_error("closure already compiled");

        SEXP result = Compiler::compileClosure(what);
        Rf_copyMostAttrib(what, result);
        return result;
    } else {
        if (TYPEOF(what) == BCODESXP) {
            what = VECTOR_ELT(CDR(what), 0);
        }
        SEXP result = Compiler::compileExpression(what);
        return result;
    }
}

REXPORT SEXP rir_markOptimize(SEXP what) {
    // TODO(mhyee): This is to mark a function for optimization.
    // However, now that we have vtables, does this still make sense? Maybe it
    // might be better to mark a specific version for optimization.
    // For now, we just mark the first version in the vtable.
    if (TYPEOF(what) != CLOSXP)
        return R_NilValue;
    SEXP b = BODY(what);
    DispatchTable* dt = DispatchTable::unpack(b);
    Function* fun = dt->first();
    fun->markOpt = true;
    return R_NilValue;
}

REXPORT SEXP rir_eval(SEXP what, SEXP env) {
    ::Function * f = isValidFunctionObject(what);
    if (f == nullptr)
        f = isValidClosureSEXP(what);
    if (f == nullptr)
        Rf_error("Not rir compiled code");
    EnvironmentProxy ep(env);
    return evalRirCode(f->body(), globalContext(), &ep);
}

REXPORT SEXP rir_body(SEXP cls) {
    ::Function * f = isValidClosureSEXP(cls);
    if (f == nullptr)
        Rf_error("Not a valid rir compiled function");
    return f->container();
}

REXPORT SEXP rir_analysis_signature(SEXP what) {
    ::Function * f = TYPEOF(what) == CLOSXP ? isValidClosureSEXP(what) : isValidFunctionSEXP(what);
    if (f == nullptr)
        Rf_error("Not a rir compiled code");
    CodeEditor ce(what);
    SignatureAnalysis sa;
    sa.analyze(ce);
    return sa.finalState().exportToR();
}


REXPORT SEXP rir_analysis_liveness(SEXP what) {
    ::Function * f = TYPEOF(what) == CLOSXP ? isValidClosureSEXP(what) : isValidFunctionSEXP(what);
    if (f == nullptr)
        Rf_error("Not a rir compiled code");
    CodeEditor ce(what);
    LivenessAnalysis la;
    la.analyze(ce);
    Rprintf("Liveness analysis dump:\n");
    for (auto i = ce.begin(); i != ce.end(); ++i) {
        Rprintf("  -- live: ");
        la[i].getState().print();
        Rprintf("\n");
        (*i).print();
    }
    return R_NilValue;
}

#include "compiler/pir_tests.h"
#include "compiler/translations/rir_2_pir/pir_compiler.h"

REXPORT SEXP pir_compile(SEXP what) {
    if (!isValidClosureSEXP(what))
        Rf_error("not a compiled closure");

    pir::Module* m = new pir::Module;
    pir::PirCompiler cmp(m);
    cmp.setVerbose(true);
    cmp.enableOptimizations();
    std::vector<SEXP> fmls;
    pir::RirInput input =
        cmp.createRirInputFromSEXP(what, fmls, m->getEnv(CLOENV(what)));
    pir::IRCode entry;
    entry.rirInput = &input;
    cmp.compile(entry);
    cmp.optimizeModule();
    delete m;
    return R_NilValue;
}

REXPORT SEXP pir_tests() {
    PirTests::run();
    return R_NilValue;
}

// startup ---------------------------------------------------------------------

bool startup() {
    initializeRuntime(rir_compile, Optimizer::reoptimizeFunction);
    return true;
}

bool startup_ok = startup();
