#include "inline.h"
#include "../pir/pir_impl.h"
#include "../transform/bb.h"
#include "../transform/replace.h"
#include "../util/cfg.h"
#include "../util/visitor.h"
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

        Visitor::run(function->entry, [&](BB* bb) {
            // Dangerous iterater usage, works since we do only update it in
            // one place.
            for (auto it = bb->begin(); it != bb->end(); it++) {
                Call* call = Call::Cast(*it);
                if (!call)
                    continue;
                auto cls = MkFunCls::Cast(call->cls());
                if (!cls)
                    continue;
                Closure* inlinee = cls->fun;
                if (inlinee->argNames.size() != call->nCallArgs())
                    continue;

                BB* split =
                    BBTransform::split(++function->maxBBId, bb, it, function);

                Call* theCall = Call::Cast(*split->begin());
                std::vector<MkArg*> arguments;
                theCall->eachCallArg([&](Value* v) {
                    MkArg* a = MkArg::Cast(v);
                    assert(a);
                    arguments.push_back(a);
                });

                // Clone the function
                BB* copy = BBTransform::clone(&function->maxBBId,
                                              inlinee->entry, function);

                // Link all inner environments to the outer one
                Visitor::run(copy, [&](BB* bb) {
                    auto ip = bb->begin();
                    while (ip != bb->end()) {
                        auto next = ip + 1;
                        auto ld = LdArg::Cast(*ip);
                        Instruction* i = *ip;
                        if (i->hasEnv() && i->env() == Env::notClosed()) {
                            i->env(cls->env());
                        }
                        if (ld) {
                            ld->replaceUsesWith(arguments[ld->id]);
                            arguments[ld->id]->type = PirType::any();
                            next = bb->remove(ip);
                        }
                        ip = next;
                    }
                });

                bb->next0 = copy;

                // Copy over promises used by the inner function
                std::vector<bool> copiedPromise;
                std::vector<size_t> newPromId;
                copiedPromise.resize(arguments.size(), false);
                newPromId.resize(arguments.size());
                Visitor::run(copy, [&](BB* bb) {
                    auto it = bb->begin();
                    while (it != bb->end()) {
                        MkArg* mk = MkArg::Cast(*it);
                        it++;
                        if (!mk)
                            continue;

                        Promise* prom = mk->prom;
                        size_t id = prom->id;
                        if (prom->fun == inlinee) {
                            if (copiedPromise[id]) {
                                mk->prom = function->promises[newPromId[id]];
                            } else {
                                Promise* clone = function->createProm();
                                BB* promCopy =
                                    BBTransform::clone(prom->entry, clone);
                                clone->id = function->promises.size();
                                function->promises.push_back(clone);
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

void Inline::apply(Closure* function) {
    TheInliner s(function);
    s();
}
}
}
