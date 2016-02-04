#ifndef PASS_H
#define PASS_H

#include "llvm.h"
#include "RIntlns.h"
#include "Intrinsics.h"
#include "JITModule.h"

namespace rjit {
namespace ir {

class Pass {
  public:
    typedef void match;

    /** Each pass must know the module of code it operates on to allow
     * constant pool access code modifications.
     */
    Pass() {}

    /** The dispatcher method. Autogenerated in some other file.
     */
    virtual bool dispatch(llvm::BasicBlock::iterator& i) {
        Pattern* ins = rjit::ir::Pattern::get(i);
        ins->advance(i);
        defaultMatch(ins);
        return true;
    }

    virtual match defaultMatch(Pattern* ins) {
        // std::cout << "default instruction match" << std::endl;
    }
};

/** Predicates mockup

  Each predicate must have the match method, that takes the same arguments as
  the function they are guarding modulo all predicate arguments. It returns true
  if the predicate approves of the matching, false if it should be denied.

  The match method is deliberately not static - first we will still have to
  create the object and pass it, so no savings there, and second, while for
  simple cases we don't need the actual object, it might be beneficial as the
  predicate may pass further information to the pass. In theory:)
 */

/** Base class for all predicates.
 */
class Predicate {};

class MockupPredicateA : public Predicate {
  public:
    bool match(Pass& h, GenericGetVar* ins) {
        return ins->symbolValue() == Rf_install("a");
    }
};

class MockupPredicateB : public Predicate {
  public:
    bool match(Pass& h, GenericGetVar* ins) {
        return ins->symbolValue() == Rf_install("b");
    }
};

namespace predicate {

template <typename T, typename W, typename... MATCH_SEQ>
class And {
  public:
    T lhs;
    W rhs;
    bool match(Pass& h, MATCH_SEQ... args) {
        return lhs.match(h, args...) and rhs.match(h, args...);
    }
};

template <typename T, typename W, typename... MATCH_SEQ>
class Or {
  public:
    T lhs;
    W rhs;
    bool match(Pass& h, MATCH_SEQ... args) {
        return lhs.match(h, args...) or rhs.match(h, args...);
    }
};
}

// Dummy Pass with empty implementation as a test case
class DummyPass : public Pass {
  public:
    DummyPass() : Pass() {}

    match gv(GenericGetVar* ins) { getVar = true; }

  public:
    bool getVar = false;

    bool dispatch(llvm::BasicBlock::iterator& i) override;
};

class MyPass : public Pass {
  public:
    MyPass() : Pass() {}

    /** Matchers are identified by their return type match - this is void
     * typedef that allows the codegen easily spot matcher methods.
     */
    match genericGetVar(GenericGetVar* ins, MockupPredicateA p) {
        std::cout << "GenericGetVar of A" << std::endl;
    }

    match genericGetVar2x(GenericGetVar* i1, GenericGetVar* i2) {
        std::cout << "Two getvars!!!!" << std::endl;
    }

    match genericGetVar(GenericGetVar* ins, MockupPredicateB p) {
        std::cout << "GenericGetVar of B" << std::endl;
    }

    match genericGetVar(GenericGetVar* ins) {
        std::cout << "GenericGetVar" << std::endl;
    }

    match genericAdd(GenericAdd* ins) {
        std::cout << "GenericAdd" << std::endl;
    }

    match ret(Return* ins) { std::cout << "Return" << std::endl; }

    match defaultMatch(Pattern* ins) override {
        std::cout << "HahaBaba" << std::endl;
    }

  public:
    bool dispatch(llvm::BasicBlock::iterator& i) override;
};

} // namespace ir
} // namespace rjit

#endif // PASS_H
