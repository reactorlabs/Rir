#include "R/Symbols.h"
#include "compiler/compiler.h"
#include "compiler/pir/pir_impl.h"
#include "compiler/util/arg_match.h"
#include "compiler/util/visitor.h"
#include "pass_definitions.h"
#include "runtime/DispatchTable.h"

namespace rir {
namespace pir {

// Try to match callsite arguments to formals
bool MatchCallArgs::apply(Compiler& cmp, ClosureVersion* cls, Code* code,
                          LogStream& log) const {
    bool anyChange = false;
    Visitor::run(code->entry, [&](BB* bb) {
        auto ip = bb->begin();
        while (ip != bb->end()) {
            auto next = ip + 1;

            if (auto calli = CallInstruction::CastCall(*ip)) {
                if (!Call::Cast(*ip) && !NamedCall::Cast(*ip)) {
                    ip = next;
                    continue;
                }

                SEXP formals = nullptr;
                ClosureVersion* target = nullptr;
                if (auto cls = calli->tryGetCls()) {
                    target = calli->tryDispatch(cls);
                    formals = cls->formals().original();
                }
                if (!target) {
                    if (auto cnst = LdConst::Cast(calli->tryGetClsArg())) {
                        if (TYPEOF(cnst->c()) == CLOSXP)
                            formals = FORMALS(cnst->c());
                    }
                    if (auto mk = MkFunCls::Cast(calli->tryGetClsArg())) {
                        formals = mk->formals;
                    }
                }

                std::vector<Value*> matchedArgs;
                ArglistOrder::CallArglistOrder argOrderOrig;
                auto call = Call::Cast(*ip);
                auto namedCall = NamedCall::Cast(*ip);
                bool staticallyArgmatched = false;

                if (formals) {
                    staticallyArgmatched = ArgumentMatcher::reorder(
                        [&](DotsList* d) {
                            ip = bb->insert(ip, d) + 1;
                            next = ip + 1;
                        },
                        formals,
                        {[&]() { return calli->nCallArgs(); },
                         [&](size_t i) { return calli->callArg(i).val(); },
                         [&](size_t i) {
                             SLOWASSERT(!namedCall ||
                                        i < namedCall->names.size());
                             return namedCall ? namedCall->names[i]
                                              : R_NilValue;
                         }},
                        matchedArgs, argOrderOrig);
                }

                Context asmpt;

                if (staticallyArgmatched) {
                    Call fake((*ip)->env(), calli->tryGetClsArg(), matchedArgs,
                              Tombstone::framestate(), (*ip)->srcIdx);
                    asmpt = fake.inferAvailableAssumptions();

                    // We can add these because arguments will be statically
                    // matched
                    asmpt.add(Assumption::StaticallyArgmatched);
                    asmpt.add(Assumption::CorrectOrderOfArguments);
                    asmpt.add(Assumption::NotTooManyArguments);
                    asmpt.add(Assumption::NoExplicitlyMissingArgs);
                    for (auto a : matchedArgs)
                        if (a == MissingArg::instance())
                            asmpt.remove(Assumption::NoExplicitlyMissingArgs);
                    asmpt.numMissing(Rf_length(formals) - matchedArgs.size());

                    if (auto cnst = LdConst::Cast(calli->tryGetClsArg())) {
                        if (DispatchTable::check(BODY(cnst->c())))
                            cmp.compileClosure(
                                cnst->c(), "unknown--fromConstant", asmpt,
                                false,
                                [&](ClosureVersion* fun) { target = fun; },
                                []() {}, {});
                    } else if (auto mk =
                                   MkFunCls::Cast(calli->tryGetClsArg())) {
                        if (auto cls = mk->tryGetCls())
                            target = cls->findCompatibleVersion(asmpt);
                        auto dt = mk->originalBody;
                        if (!target && dt) {
                            auto srcRef = mk->srcRef;
                            cmp.compileFunction(dt, "unknown--fromMkFunCls",
                                                formals, srcRef, asmpt,
                                                [&](ClosureVersion* fun) {
                                                    mk->setCls(fun->owner());
                                                    target = fun;
                                                },
                                                []() {}, {});
                        }
                    }
                }

                if (staticallyArgmatched && target) {
                    anyChange = true;
                    if (auto c = call) {
                        auto nc = new StaticCall(
                            c->env(), target->owner(), asmpt, matchedArgs,
                            std::move(argOrderOrig), c->frameStateOrTs(),
                            c->srcIdx, c->cls()->followCastsAndForce());
                        (*ip)->replaceUsesAndSwapWith(nc, ip);
                    } else if (auto c = namedCall) {
                        auto nc = new StaticCall(
                            c->env(), target->owner(), asmpt, matchedArgs,
                            std::move(argOrderOrig), c->frameStateOrTs(),
                            c->srcIdx, c->cls()->followCastsAndForce());
                        (*ip)->replaceUsesAndSwapWith(nc, ip);
                    } else {
                        assert(false);
                    }
                }
            }

            ip = next;
        }
    });
    return anyChange;
}

} // namespace pir
} // namespace rir
