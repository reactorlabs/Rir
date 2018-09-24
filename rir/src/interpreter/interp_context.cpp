#include "interp_context.h"
#include "api.h"
#include "runtime.h"

void initializeResizeableList(ResizeableList* l, size_t capacity, SEXP parent,
                              size_t index) {
    l->capacity = capacity;
    l->list = Rf_allocVector(VECSXP, capacity);
    SET_VECTOR_ELT(parent, index, l->list);
    rl_setLength(l, 0);
}

SEXP R_Subset2Sym;
SEXP R_SubsetSym;
SEXP R_SubassignSym;
SEXP R_Subassign2Sym;
SEXP R_valueSym;
SEXP setterPlaceholderSym;
SEXP getterPlaceholderSym;
SEXP quoteSym;

Context* context_create() {
    Context* c = new Context;
    c->list = Rf_allocVector(VECSXP, 2);
    R_PreserveObject(c->list);
    initializeResizeableList(&c->cp, POOL_CAPACITY, c->list, CONTEXT_INDEX_CP);
    initializeResizeableList(&c->src, POOL_CAPACITY, c->list,
                             CONTEXT_INDEX_SRC);
    // first item in source and constant pools is R_NilValue so that we can use
    // the index 0 for other purposes
    src_pool_add(c, R_NilValue);
    cp_pool_add(c, R_NilValue);
    R_Subset2Sym = Rf_install("[[");
    R_SubsetSym = Rf_install("[");
    R_SubassignSym = Rf_install("[<-");
    R_Subassign2Sym = Rf_install("[[<-");
    R_valueSym = Rf_install("value");
    setterPlaceholderSym = Rf_install("*.placeholder.setter.*");
    getterPlaceholderSym = Rf_install("*.placeholder.getter.*");
    quoteSym = Rf_install("quote");

    auto pir = getenv("PIR_ENABLE");

    c->exprCompiler = rir_compile;
    c->closureCompiler = [](SEXP closure, SEXP name) {
        return rir_compile(closure, R_NilValue);
    };
    c->closureOptimizer = [](SEXP f, SEXP n) { return f; };

    if (pir && std::string(pir).compare("off") == 0) {
        // do nothing; use defaults
    } else if (pir && std::string(pir).compare("force") == 0) {
        c->closureCompiler = [](SEXP f, SEXP n) {
            SEXP rir = rir_compile(f, R_NilValue);
            return pirOptDefaultOpts(rir, n);
        };
    } else if (pir && std::string(pir).compare("force_dryrun") == 0) {
        c->closureCompiler = [](SEXP f, SEXP n) {
            SEXP rir = rir_compile(f, R_NilValue);
            return pirOptDefaultOptsDryrun(rir, n);
        };
    } else {
        c->closureOptimizer = pirOptDefaultOpts;
    }

    return c;
}

extern Context* globalContext_;
