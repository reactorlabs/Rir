#include "inline.h"
#include "../pir/pir_impl.h"
#include "../transform/bb.h"
#include "../transform/replace.h"
#include "../util/cfg.h"
#include "../util/visitor.h"
#include "R/Funtab.h"
#include "R/Symbols.h"
#include "R/r.h"

#include <algorithm>
#include <unordered_map>

namespace {

using namespace rir::pir;

class TheInliner {
  public:
    Closure* function;
    TheInliner(Closure* function) : function(function) {}

    void operator()() {
        size_t fuel = 5;

        Visitor::run(function->entry, [&](BB* bb) {
            // Dangerous iterater usage, works since we do only update it in
            // one place.
            for (auto it = bb->begin(); it != bb->end() && fuel; it++) {
                auto call = CallInstruction::CastCall(*it);
                if (!call)
                    continue;

                Closure* inlinee = nullptr;
                Value* staticEnv = nullptr;

                if (auto call = Call::Cast(*it)) {
                    auto mkcls = MkFunCls::Cast(call->cls()->baseValue());
                    if (!mkcls)
                        continue;
                    inlinee = mkcls->fun;
                    if (inlinee->argNames.size() != call->nCallArgs())
                        continue;
                    staticEnv = mkcls->lexicalEnv();
                } else if (auto call = StaticCall::Cast(*it)) {
                    inlinee = call->cls();
                    // if we don't know the closure of the inlinee, we can't
                    // inline.
                    if (inlinee->closureEnv() == Env::notClosed() &&
                        inlinee != function)
                        continue;
                    // TODO: honestly I have no clue how namespaces work. For
                    // now if we hit any env with attribs, we do not inline.
                    if (ATTRIB(inlinee->closureEnv()->rho) != R_NilValue ||
                        R_IsNamespaceEnv(inlinee->closureEnv()->rho))
                        continue;
                    staticEnv = inlinee->closureEnv();
                } else {
                    continue;
                }

                fuel--;

                BB* split =
                    BBTransform::split(function->nextBBId++, bb, it, function);
                auto theCall = *split->begin();
                auto theCallInstruction = CallInstruction::CastCall(theCall);
                std::vector<Value*> arguments;
                theCallInstruction->eachCallArg(
                    [&](Value* v) { arguments.push_back(v); });
                auto callerSafepoint =
                    split->size() > 1 ? Safepoint::Cast(*(split->begin() + 1))
                                      : nullptr;

                // Clone the function
                BB* copy = BBTransform::clone(inlinee->entry, function);

                bool needsEnvPatching = inlinee->closureEnv() != staticEnv;

                bool fail = false;
                Visitor::run(copy, [&](BB* bb) {
                    auto ip = bb->begin();
                    while (!fail && ip != bb->end()) {
                        auto next = ip + 1;
                        auto ld = LdArg::Cast(*ip);
                        Instruction* i = *ip;
                        // We should never inline UseMethod
                        if (auto ld = LdFun::Cast(i)) {
                            if (ld->varName == rir::symbol::UseMethod) {
                                fail = true;
                                return;
                            }
                        }
                        if (auto sp = Safepoint::Cast(i)) {
                            if (!sp->next() && !callerSafepoint) {
                                fail = true;
                                return;
                            }

                            // TODO: support chained safepoints in backend and
                            // rir
                            return;

                            // When inlining a safepoint we need to chain it
                            // with the safepoints after the call to the
                            // inlinee
                            Safepoint* prevSp = sp;
                            Safepoint* nextSp = callerSafepoint;
                            size_t created = 0;
                            while (nextSp) {
                                auto clone = Safepoint::Cast(nextSp->clone());
                                // Remove the return value of the inlinee
                                if (nextSp == callerSafepoint) {
                                    clone->popArg();
                                    clone->stackSize--;
                                }
                                // Insert the safepoint
                                ip = bb->insert(ip, clone);
                                created++;
                                prevSp->next(clone);
                                prevSp = nextSp;
                                nextSp = nextSp->next();
                            }
                            next = ip + created + 1;
                        }
                        // If the inlining resolved some env, we need to
                        // update. For example this happens if we inline an
                        // inner function. Then the lexical env is the current
                        // functions env.
                        if (needsEnvPatching && i->accessesEnv() &&
                            i->env() == inlinee->closureEnv()) {
                            i->env(staticEnv);
                        }
                        if (ld) {
                            Value* a = arguments[ld->id];
                            if (MkArg::Cast(a)) {
                                // We need to cast from a promise to a lazy
                                // value
                                auto cast = new CastType(a, RType::prom,
                                                         PirType::any());
                                ip = bb->insert(ip + 1, cast);
                                ip--;
                                a = cast;
                            }
                            ld->replaceUsesWith(a);
                            next = bb->remove(ip);
                        }
                        ip = next;
                    }
                });

                if (fail) {
                    delete copy;
                    bb->overrideNext(split);

                } else {

                    bb->overrideNext(copy);

                    // Copy over promises used by the inner function
                    std::vector<bool> copiedPromise;
                    std::vector<size_t> newPromId;
                    copiedPromise.resize(inlinee->promises.size(), false);
                    newPromId.resize(inlinee->promises.size());
                    Visitor::run(copy, [&](BB* bb) {
                        auto it = bb->begin();
                        while (it != bb->end()) {
                            MkArg* mk = MkArg::Cast(*it);
                            it++;
                            if (!mk)
                                continue;

                            size_t id = mk->prom->id;
                            if (mk->prom->fun == inlinee) {
                                assert(id < copiedPromise.size());
                                if (copiedPromise[id]) {
                                    mk->prom =
                                        function->promises[newPromId[id]];
                                } else {
                                    Promise* clone = function->createProm(
                                        mk->prom->srcPoolIdx);
                                    BB* promCopy = BBTransform::clone(
                                        mk->prom->entry, clone);
                                    clone->entry = promCopy;
                                    newPromId[id] = clone->id;
                                    copiedPromise[id] = true;
                                    mk->prom = clone;
                                }
                            }
                        }
                    });

                    Value* inlineeRes = BBTransform::forInline(copy, split);
                    theCall->replaceUsesWith(inlineeRes);

                    // Remove the call instruction
                    split->remove(split->begin());
                }

                bb = split;
                it = split->begin();

                // Can happen if split only contained the call instruction
                if (it == split->end())
                    break;
            }
        });
    }
};
}

namespace rir {
namespace pir {

void Inline::apply(Closure* function) const {
    TheInliner s(function);
    s();
}
}
}
