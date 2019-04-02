#ifndef API_H_
#define API_H_

#include "R/r.h"
#include "compiler/debugging/debugging.h"
#include "runtime/Assumptions.h"
#include <stdint.h>

#define REXPORT extern "C"

extern int R_ENABLE_JIT;
extern rir::pir::DebugOptions PirDebug;

REXPORT SEXP rir_invocation_count(SEXP what);
REXPORT SEXP rir_eval(SEXP exp, SEXP env);
REXPORT SEXP pir_compile(SEXP closure, SEXP name, SEXP debugFlags,
                         SEXP debugStyle);
REXPORT SEXP rir_compile(SEXP what, SEXP env);
REXPORT SEXP pir_tests();
REXPORT SEXP pir_check(SEXP f, SEXP check, SEXP env);
REXPORT SEXP pir_setDebugFlags(SEXP debugFlags);
SEXP pirCompile(SEXP closure, const rir::Assumptions& assumptions,
                const std::string& name, const rir::pir::DebugOptions& debug);
extern SEXP rirOptDefaultOpts(SEXP closure, const rir::Assumptions&, SEXP name);
extern SEXP rirOptDefaultOptsDryrun(SEXP closure, const rir::Assumptions&,
                                    SEXP name);

#endif // API_H_
