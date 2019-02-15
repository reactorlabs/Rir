#ifndef LAZY_ENVIRONMENT_H
#define LAZY_ENVIRONMENT_H

#include "../ir/BC_inc.h"
#include "RirDataWrapper.h"
#include <cassert>
#include <cstdint>
#include <functional>

SEXP createEnvironment(const std::vector<SEXP>*, const SEXP, const rir::Opcode*,
                       Context*, R_bcstack_t*, SEXP);
namespace rir {

#define LAZY_ENVIRONMENT_MAGIC 0xe4210e47

/**
 * EnvironmentStub holds the information needed to create an
 * environment lazily.
 */
struct LazyEnvironment
    : public RirDataWrapper<LazyEnvironment, LAZY_ENVIRONMENT_MAGIC> {
    LazyEnvironment() = delete;
    LazyEnvironment(const LazyEnvironment&) = delete;
    LazyEnvironment& operator=(const LazyEnvironment&) = delete;

    // We need to store the arguments + the parent + *opcode for the names
    LazyEnvironment(std::vector<SEXP>* arguments, SEXP parent, Opcode* opcode,
                    Context* ctx, R_bcstack_t* localsBase)
        : RirDataWrapper(5), arguments_(arguments), parent_(parent),
          names_(opcode), ctx_(ctx), localsBase_(localsBase){};
    ~LazyEnvironment() { delete arguments_; }

    const std::vector<SEXP>* arguments() { return arguments_; }
    const SEXP parent() { return parent_; }
    const Opcode* names() { return names_; }
    Context* ctx() { return ctx_; }
    R_bcstack_t* localsBase() { return localsBase_;}

    SEXP create() {
        return createEnvironment(arguments(), parent(), names(), ctx(), localsBase(),
                                 (SEXP)this);
    }

  private:
    const std::vector<SEXP>* arguments_;
    const SEXP parent_;
    const Opcode* names_;
    Context* ctx_;
    R_bcstack_t* localsBase_;
};
} // namespace rir

#endif
