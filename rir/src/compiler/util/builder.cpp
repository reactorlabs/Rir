#include "builder.h"
#include "../pir/pir_impl.h"

namespace rir {
namespace pir {

BB* Builder::createBB() { return new BB(code, code->nextBBId++); }

void Builder::markDone(BB* bb) {
    if (done.size() <= bb->id)
        done.resize(bb->id + 1);
    done[bb->id] = true;
}

bool Builder::isDone(BB* bb) const {
    if (done.size() <= bb->id)
        return false;
    return done[bb->id];
}

void Builder::setBranch(BB* next0, BB* next1) {
    assert(bb);
    assert(!bb->isEmpty() && Branch::Cast(bb->last()));
    markDone(bb);
    bb->setBranch(next0, next1);
}

void Builder::setNext(BB* next) {
    assert(bb);
    markDone(bb);
    bb->setNext(next);
}

void Builder::enterBB(BB* next) {
    assert(!isDone(next));
    if (bb)
        markDone(bb);
    bb = next;
}

void Builder::createNextBB() {
    auto n = createBB();
    if (bb)
        setNext(n);
    bb = n;
}

void Builder::add(Instruction* i) {
    switch (i->tag) {
    case Tag::_UNUSED_:
        assert(false && "Invalid instruction");
    case Tag::PirCopy:
    case Tag::CallImplicit:
    case Tag::ScheduledDeopt:
        assert(false && "This instruction is only allowed during lowering");
    default: {}
    }
    bb->append(i);
}

FrameState* Builder::registerFrameState(rir::Code* srcCode, Opcode* pos,
                                        const RirStack& stack) {
    auto sp = new FrameState(env, srcCode, pos, stack);
    add(sp);
    return sp;
};

void Builder::conditionalDeopt(Value* condition, rir::Code* srcCode,
                               Opcode* pos, const RirStack& stack,
                               bool deoptOnFalseBranch) {
    add(new Branch(condition));
    auto cont = createBB();
    auto fail = createBB();
    // We may deoptimize if the condition holds or unless the condition holds
    if (deoptOnFalseBranch)
        setBranch(cont, fail);
    else
        setBranch(fail, cont);

    enterBB(fail);
    auto sp = registerFrameState(srcCode, pos, stack);
    add(new Deopt(sp));

    enterBB(cont);
};

Builder::Builder(Closure* fun, Value* closureEnv)
    : function(fun), code(fun), env(nullptr) {
    createNextBB();
    assert(!function->entry);
    function->entry = bb;
    std::vector<Value*> args(fun->argNames.size());
    for (long i = fun->argNames.size() - 1; i >= 0; --i)
        args[i] = this->operator()(new LdArg(i));
    auto mkenv = new MkEnv(closureEnv, fun->argNames, args.data());
    add(mkenv);
    this->env = mkenv;
}

Builder::Builder(Closure* fun, Promise* prom)
    : function(fun), code(prom), env(nullptr) {
    createNextBB();
    assert(!prom->entry);
    prom->entry = bb;
    auto ldenv = new LdFunctionEnv();
    add(ldenv);
    this->env = ldenv;
}
} // namespace pir
} // namespace rir
