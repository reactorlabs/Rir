#include "../analysis/available_checkpoints.h"
#include "../pir/pir_impl.h"
#include "../transform/bb.h"
#include "../transform/replace.h"
#include "../translations/rir_2_pir/rir_2_pir_compiler.h"
#include "../util/cfg.h"
#include "../util/safe_builtins_list.h"
#include "../util/visitor.h"
#include "R/Funtab.h"
#include "R/Symbols.h"
#include "R/r.h"
#include "pass_definitions.h"

#include <unordered_map>

namespace rir {
namespace pir {

void EagerCalls::apply(RirCompiler& cmp, ClosureVersion* closure,
                       LogStream& log) const {
    auto code = closure->entry;
    AvailableCheckpoints checkpoint(closure, closure, log);

    auto replaceLdFunBuiltinWithDeopt = [&](BB* bb, BB::Instrs::iterator ip,
                                            Checkpoint* cp, SEXP builtin,
                                            LdFun* ldfun) {
        assert(LdFun::Cast(*ip));
        assert(cp);

        // skip ldfun
        ip++;

        auto expected = new LdConst(builtin);
        ip = bb->insert(ip, expected);
        ip++;
        Instruction* given = ldfun;

        // We use ldvar instead of ldfun for the guard. The reason is that ldfun
        // can force promises, which is a pain for our optimizer to deal with.
        // Note that ldvar is conservative. If we find a non-function binding
        // with the same name, we will deopt unneccessarily. In the case of `c`
        // this is guaranteed to cause problems, since many variables are called
        // "c". Therefore we keep the ldfun in this case, unless we already know
        // that the function "c" comes from the global env.
        // TODO: Implement this with a dependency on the binding cell instead of
        // an eager check.
        auto funEnv = Env::Cast(ldfun->env());
        if (ldfun->varName != symbol::c ||
            (funEnv && funEnv->rho == R_GlobalEnv)) {
            given = new LdVar(ldfun->varName, ldfun->env());
            ip = bb->insert(ip, given);
            ip++;
        }

        auto test = new Identical(given, expected);
        ip = bb->insert(ip, test);
        ip++;

        auto assume = new Assume(test, cp);
        ip = bb->insert(ip, assume);
        ip++;

        return ip;
    };

    auto replaceCallWithCallBuiltin = [](BB* bb, BB::Instrs::iterator ip,
                                         Call* call, SEXP builtin) {
        std::vector<Value*> args;
        call->eachCallArg([&](Value* a) {
            if (auto mk = MkArg::Cast(a->followCasts())) {
                if (mk->isEager()) {
                    args.push_back(mk->eagerArg());
                } else {
                    auto asArg = new CastType(mk, CastType::Upcast, RType::prom,
                                              PirType::valOrLazy());
                    auto forced = new Force(asArg, call->env());
                    ip = bb->insert(ip, forced);
                    ip = bb->insert(ip, asArg);
                    args.push_back(forced);
                    ip += 2;
                }
            } else if (a->type.maybePromiseWrapped()) {
                ip = bb->insert(ip, new Force(a, call->env()));
                args.push_back(*ip);
                ip++;
            } else {
                args.push_back(a);
            }
        });
        auto bt =
            BuiltinCallFactory::New(call->env(), builtin, args, call->srcIdx);
        call->replaceUsesWith(bt);
        bb->replace(ip, bt);
        return ip;
    };

    // Search for calls that likely point to a builtin.
    std::unordered_map<LdFun*, std::pair<SEXP, Checkpoint*>> needsGuard;
    Visitor::run(code, [&](BB* bb) {
        auto ip = bb->begin();
        while (ip != bb->end()) {
            if (auto call = Call::Cast(*ip)) {
                if (auto ldfun = LdFun::Cast(call->cls())) {
                    if (ldfun->hint) {
                        if (TYPEOF(ldfun->hint) == BUILTINSXP) {
                            // We can only speculate if we have a checkpoint at
                            // the ldfun position, since we want to deopt before
                            // forcing arguments.
                            if (auto cp = checkpoint.at(ldfun)) {
                                ip = replaceCallWithCallBuiltin(bb, ip, call,
                                                                ldfun->hint);
                                needsGuard[ldfun] = {ldfun->hint, cp};
                            }
                        }
                    } else {
                        // Only speculate if we don't have a static guess
                        if (!ldfun->guessedBinding()) {
                            auto env =
                                Env::Cast(closure->owner()->closureEnv());
                            if (env != Env::notClosed() && env->rho) {
                                auto name = ldfun->varName;
                                auto builtin = Rf_findVar(name, env->rho);
                                if (TYPEOF(builtin) == PROMSXP)
                                    builtin = PRVALUE(builtin);
                                if (TYPEOF(builtin) == BUILTINSXP) {
                                    auto rho = env->rho;
                                    bool inBase = false;
                                    if (rho == R_BaseEnv ||
                                        rho == R_BaseNamespace) {
                                        inBase =
                                            SYMVALUE(name) == builtin &&
                                            SafeBuiltinsList::
                                                assumeStableInBaseEnv(name);
                                    }

                                    auto cp = checkpoint.at(ldfun);

                                    if (inBase || cp) {
                                        ip = replaceCallWithCallBuiltin(
                                            bb, ip, call, builtin);
                                        if (!inBase)
                                            needsGuard[ldfun] = {builtin, cp};
                                    }
                                }
                            }
                        }
                    }
                }
            }

            ip++;
        }
    });

    Visitor::run(code, [&](BB* bb) {
        auto ip = bb->begin();
        while (ip != bb->end()) {
            auto next = ip + 1;

            // Insert the actual guards needed for the above transformation
            if (auto ldfun = LdFun::Cast(*ip)) {
                auto r = needsGuard.find(ldfun);
                if (r != needsGuard.end()) {
                    ip = replaceLdFunBuiltinWithDeopt(bb, ip, r->second.second,
                                                      r->second.first, ldfun);
                    needsGuard.erase(r);
                    continue;
                }
            }

            // Look for static calls, where we statically know that all (or
            // some) arguments are eager. In this case we will compile an
            // special eager version of the function and call this one instead.
            if (auto call = StaticCall::Cast(*ip)) {
                Closure* cls = call->cls();
                ClosureVersion* version = call->tryDispatch();

                if (!version || call->nCallArgs() != cls->nargs()) {
                    ip = next;
                    continue;
                }

                auto availableAssumptions = call->inferAvailableAssumptions();

                // We picked up more assumptions, let's compile a better
                // version. Maybe we should limit this at some point, to avoid
                // version explosion.
                if (cls->nargs() > 0 &&
                    availableAssumptions.includes(
                        Assumption::NoReflectiveArgument) &&
                    !version->assumptions().includes(
                        Assumption::NoReflectiveArgument)) {
                    auto newVersion = cls->cloneWithAssumptions(
                        version, availableAssumptions,
                        [&](ClosureVersion* newCls) {
                            Visitor::run(newCls->entry, [&](Instruction* i) {
                                if (auto f = Force::Cast(i)) {
                                    if (LdArg::Cast(f)) {
                                        f->elideEnv();
                                        f->effects.reset(
                                            Effect::DependsOnAssume);
                                        f->effects.reset(Effect::Reflection);
                                    }
                                }
                            });
                        });
                    call->hint = newVersion;
                    assert(call->tryDispatch() == newVersion);
                    ip = next;
                    continue;
                }

                bool allEager = version->properties.includes(
                    ClosureVersion::Property::IsEager);
                if (!allEager &&
                    version->properties.argumentForceOrder.empty()) {
                    ip = next;
                    continue;
                }

                auto isEager = [&](size_t i) {
                    if (allEager)
                        return true;
                    for (auto a : version->properties.argumentForceOrder) {
                        // We know that a is forced before i, therefore we are
                        // not in left-to-right order
                        // TODO: support reordering of the evaluation
                        if (a > i)
                            return false;
                        // We found the argument in the list of certainly forced
                        // promises
                        if (a == i)
                            return true;
                    }
                    return false;
                };

                bool noMissing = true;
                call->eachCallArg([&](Value* v) {
                    if (!MkArg::Cast(v))
                        noMissing = false;
                });
                if (!noMissing) {
                    ip = next;
                    continue;
                }

                // Remember which arguments are already expected to be eager by
                // the callee and ensure that we will evaluate them eagerly
                // below.
                call->eachCallArg([&](InstrArg& arg) {
                    if (auto mk = MkArg::Cast(arg.val())) {
                        if (mk->isEager()) {
                            arg.val() = mk->eagerArg();
                        } else {
                            auto asArg =
                                new CastType(mk, CastType::Upcast, RType::prom,
                                             PirType::valOrLazy());
                            auto forced = new Force(asArg, call->env());
                            arg.val() = forced;
                            ip = bb->insert(ip, forced);
                            ip = bb->insert(ip, asArg);
                            ip++;
                        }
                    }
                });
                next = ip + 1;

                Assumptions newAssumptions = availableAssumptions;
                for (size_t i = 0; i < call->nCallArgs(); ++i) {
                    if (!newAssumptions.isEager(i) && isEager(i))
                        newAssumptions.setEager(i);
                }
                // This might fire back, since we don't know if we really have no
                // objects... We should have some profiling. It's still sound, since
                // static_call_ will check the assumptions
                for (size_t i = 0; i < call->nCallArgs(); ++i)
                    if (!newAssumptions.isNotObj(i) &&
                        newAssumptions.isEager(i))
                        newAssumptions.setNotObj(i);

                auto newVersion = cls->cloneWithAssumptions(
                    version, newAssumptions, [&](ClosureVersion* newCls) {
                        Visitor::run(newCls->entry, [&](Instruction* i) {
                            if (auto ld = LdArg::Cast(i)) {
                                if (isEager(ld->id)) {
                                    ld->type = PirType::promiseWrappedVal()
                                                   .notObject()
                                                   .notMissing();
                               }
                            }
                        });
                    });
                call->hint = newVersion;
            }
            ip = next;
        }
    });

    // Third step: eagerly evaluate arguments if we know from above that we will
    // call a function that expects them to be eager.
    Visitor::run(code, [&](BB* bb) {
        for (auto ip = bb->begin(); ip != bb->end(); ++ip) {
            if (auto call = StaticCall::Cast(*ip)) {
                auto version = call->tryDispatch();
                if (version && version->properties.includes(
                                   ClosureVersion::Property::NoReflection)) {
                    call->eachCallArg([&](InstrArg& arg) {
                        if (auto mk = MkArg::Cast(arg.val())) {
                            if (mk->isEager()) {
                                arg.val() = mk->eagerArg();
                            }
                        }
                    });
                }
            }
        }
    });
}
} // namespace pir
} // namespace rir
