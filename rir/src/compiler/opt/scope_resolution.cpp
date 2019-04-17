#include "../analysis/query.h"
#include "../analysis/scope.h"
#include "../pir/pir_impl.h"
#include "../util/cfg.h"
#include "../util/phi_placement.h"
#include "../util/safe_builtins_list.h"
#include "../util/visitor.h"
#include "R/r.h"
#include "pass_definitions.h"
#include "utils/Set.h"

#include <algorithm>
#include <unordered_map>

namespace {

using namespace rir::pir;
class TheScopeResolution {
  public:
    ClosureVersion* function;
    CFG cfg;
    DominanceGraph dom;
    DominanceFrontier dfront;
    LogStream& log;
    explicit TheScopeResolution(ClosureVersion* function, LogStream& log)
        : function(function), cfg(function), dom(function),
          dfront(function, cfg, dom), log(log) {}

    void operator()() {
        ScopeAnalysis analysis(function, log);
        analysis();
        auto& finalState = analysis.result();
        if (finalState.noReflection())
            function->properties.set(ClosureVersion::Property::NoReflection);

        std::unordered_map<Value*, Value*> replacedValue;
        auto getReplacedValue = [&](Value* val) {
            while (replacedValue.count(val))
                val = replacedValue.at(val);
            return val;
        };
        auto getSingleLocalValue =
            [&](const AbstractPirValue& result) -> Value* {
            if (!result.isSingleValue())
                return nullptr;
            if (result.singleValue().recursionLevel != 0)
                return nullptr;
            auto val = getReplacedValue(result.singleValue().val);
            assert(val->validIn(function));
            return val;
        };

        auto tryInsertPhis = [&](AbstractPirValue res, BB* bb,
                                 BB::Instrs::iterator& iter) -> Value* {
            if (res.isUnknown())
                return nullptr;

            auto iterPos = *iter;
            bool onlyLocalVals = true;
            res.eachSource([&](const ValOrig& src) {
                if (src.recursionLevel > 0)
                    onlyLocalVals = false;
            });
            if (!onlyLocalVals)
                return nullptr;

            std::unordered_map<BB*, Value*> inputs;
            bool fail = false;
            res.eachSource([&](ValOrig v) {
                auto i = Instruction::Cast(v.val);
                if (fail)
                    return;
                if (!i || inputs.count(v.origin->bb()))
                    fail = true;
                inputs[v.origin->bb()] = i;
            });
            if (fail)
                return nullptr;

            auto pl = PhiPlacement(function, bb, inputs, cfg, dom, dfront);
            if (!pl.success)
                return nullptr;

            auto findExistingPhi =
                [&](BB* pos,
                    const rir::SmallSet<PhiPlacement::PhiInput>& inps) -> Phi* {
                for (auto i : *pos) {
                    if (auto candidate = Phi::Cast(i)) {
                        if (candidate->nargs() == inps.size()) {
                            rir::SmallSet<PhiPlacement::PhiInput> candidateInp;
                            // This only works for value inputs, not for phi
                            // inputs. I am not sure how it could be extended...
                            candidate->eachArg([&](BB* inbb, Value* inval) {
                                candidateInp.insert({inbb, nullptr, inval});
                            });
                            if (candidateInp == inps)
                                return candidate;
                        };
                    }
                }
                return nullptr;
            };

            std::unordered_map<BB*, Phi*> thePhis;
            for (auto& phi : pl.placement) {
                if (auto p = findExistingPhi(phi.first, phi.second))
                    thePhis[phi.first] = p;
                else
                    thePhis[phi.first] = new Phi;
            }

            for (auto& computed : pl.placement) {
                auto& pos = computed.first;
                auto& phi = thePhis.at(pos);

                // Existing phi
                if (phi->nargs() > 0)
                    continue;

                assert(computed.second.size() > 1);
                for (auto& p : computed.second) {
                    if (p.aValue)
                        phi->addInput(p.inputBlock, getReplacedValue(p.aValue));
                    else
                        phi->addInput(p.inputBlock, thePhis.at(p.otherPhi));
                };

                pos->insert(pos->begin(), phi);
                // If the insert changed the current bb, we need to keep the
                // iterator updated
                if (pos == bb)
                    iter = bb->atPosition(iterPos);

                if (!phi->type.isA(res.type))
                    phi->type = res.type;
            }

            for (auto& phi : thePhis)
                phi.second->updateType();

            return thePhis.at(pl.targetPhiPosition);
        };

        Visitor::run(function->entry, [&](BB* bb) {
            auto ip = bb->begin();
            while (ip != bb->end()) {
                Instruction* i = *ip;
                auto next = ip + 1;

                auto before = analysis.at<ScopeAnalysis::BeforeInstruction>(i);
                auto after = analysis.at<ScopeAnalysis::AfterInstruction>(i);

                // Force and callees can only see our env only through
                // reflection
                if (i->hasEnv() &&
                    (CallInstruction::CastCall(i) || Force::Cast(i))) {
                    if (after.noReflection()) {
                        i->elideEnv();
                        i->effects.reset(Effect::Reflection);
                    }
                    if (after.envNotEscaped(i->env())) {
                        i->effects.reset(Effect::LeaksEnv);
                    }
                }

                // StVarSuper where the parent environment is known and
                // local, can be replaced by simple StVar, if the variable
                // exists in the super env.
                if (auto sts = StVarSuper::Cast(i)) {
                    auto aLoad =
                        analysis.superLoad(before, sts->varName, sts->env());
                    if (aLoad.env != AbstractREnvironment::UnknownParent &&
                        !aLoad.result.isUnknown() &&
                        aLoad.env->validIn(function)) {
                        auto r = new StVar(sts->varName, sts->val(), aLoad.env);
                        bb->replace(ip, r);
                        sts->replaceUsesWith(r);
                        replacedValue[sts] = r;
                    }
                    ip = next;
                    continue;
                }

                // Constant fold "missing" if we can
                if (auto missing = Missing::Cast(i)) {
                    auto res =
                        analysis.load(before, missing->varName, missing->env());
                    if (!res.result.type.maybeMissing()) {
                        // Missing still returns TRUE, if the argument was
                        // initially missing, but then overwritten by a default
                        // argument. Currently our analysis cannot really
                        // distinguish those cases. Therefore, if the current
                        // value of the variable is guaranteed to not be a
                        // missing value, we additionally need verify that the
                        // initial argument (the argument to the mkenv) was also
                        // guaranteed to not be a missing value.
                        // TODO: this is a bit brittle and might break as soon
                        // as we start improving the handling of missing args in
                        // MkEnv.
                        if (auto env = MkEnv::Cast(missing->env())) {
                            bool initiallyMissing = false;
                            env->eachLocalVar([&](SEXP name, Value* val) {
                                if (name == missing->varName)
                                    initiallyMissing = val->type.maybeMissing();
                            });
                            if (!initiallyMissing) {
                                auto theFalse = new LdConst(R_FalseValue);
                                missing->replaceUsesAndSwapWith(theFalse, ip);
                                replacedValue[missing] = theFalse;
                            }
                        }
                    } else {
                        res.result.ifSingleValue([&](Value* v) {
                            if (v == MissingArg::instance()) {
                                auto theTruth = new LdConst(R_TrueValue);
                                missing->replaceUsesAndSwapWith(theTruth, ip);
                                replacedValue[missing] = theTruth;
                            }
                        });
                    }
                }

                if (bb->isDeopt()) {
                    if (auto fs = FrameState::Cast(i)) {
                        if (auto mk = MkEnv::Cast(fs->env())) {
                            if (mk->context == 1 && !mk->stub &&
                                mk->bb() != bb &&
                                mk->usesAreOnly(
                                    function->entry,
                                    {Tag::FrameState, Tag::StVar})) {
                                analysis.tryMaterializeEnv(
                                    before, mk,
                                    [&](const std::unordered_map<
                                        SEXP, AbstractPirValue>& env) {
                                        std::vector<SEXP> names;
                                        std::vector<Value*> values;
                                        for (auto& e : env) {
                                            names.push_back(e.first);
                                            if (e.second.isUnknown())
                                                return;
                                            if (auto val = getSingleLocalValue(
                                                    e.second)) {
                                                values.push_back(val);
                                            } else {
                                                auto phi = tryInsertPhis(
                                                    e.second, bb, ip);
                                                if (!phi)
                                                    return;
                                                values.push_back(phi);
                                            }
                                        }
                                        auto deoptEnv =
                                            new MkEnv(mk->lexicalEnv(), names,
                                                      values.data());
                                        ip = bb->insert(ip, deoptEnv);
                                        ip++;
                                        next = ip + 1;
                                        mk->replaceUsesWithLimits(deoptEnv, bb);
                                    });
                            }
                        }
                    }
                }

                analysis.lookupAt(after, i, [&](const AbstractLoad& aLoad) {
                    auto& res = aLoad.result;

                    bool isActualLoad =
                        LdVar::Cast(i) || LdFun::Cast(i) || LdVarSuper::Cast(i);

                    // In case the scope analysis is sure that this is
                    // actually the same as some other PIR value. So let's just
                    // replace it.
                    if (res.isSingleValue()) {
                        if (auto val = getSingleLocalValue(res)) {
                            if (val->type.isA(i->type)) {
                                if (isActualLoad && val->type.maybeMissing()) {
                                    // LdVar checks for missingness, so we need
                                    // to preserve this.
                                    auto chk = new ChkMissing(val);
                                    ip = bb->insert(ip, chk);
                                    ip++;
                                    val = chk;
                                }
                                replacedValue[i] = val;
                                i->replaceUsesWith(val);
                                next = bb->remove(ip);
                                return;
                            }
                        }
                    }

                    // Narrow down type according to what the analysis reports
                    if (i->type.isRType()) {
                        auto inferedType = res.type;
                        if (!i->type.isA(inferedType))
                            i->type = inferedType;
                    }

                    // The generic case where we have a bunch of potential
                    // values we will insert a phi to group all of them. In
                    // general this is only possible if they all come from the
                    // current function (and not through inter procedural
                    // analysis from other functions).
                    //
                    // Also, we shold only do this for actual loads and not
                    // in general. Otherwise there is a danger that we insert
                    // the same phi twice (e.g. if a force returns the result
                    // of a load, we will resolve the load and the force) which
                    // ends up being rather painful.
                    if (!res.isUnknown() && isActualLoad) {
                        if (auto resPhi = tryInsertPhis(res, bb, ip)) {
                            Value* val = resPhi;
                            if (val->type.maybeMissing()) {
                                // LdVar checks for missingness, so we need
                                // to preserve this.
                                auto chk = new ChkMissing(val);
                                ip = bb->insert(ip, chk);
                                ip++;
                                val = chk;
                            }
                            i->replaceUsesWith(val);
                            replacedValue[i] = val;
                            next = bb->remove(ip);
                            return;
                        }
                    }

                    // LdVarSuper where the parent environment is known and
                    // local, can be replaced by a simple LdVar
                    if (auto lds = LdVarSuper::Cast(i)) {
                        auto e = Env::parentEnv(lds->env());
                        if (e) {
                            auto r = new LdVar(lds->varName, e);
                            bb->replace(ip, r);
                            lds->replaceUsesWith(r);
                            replacedValue[lds] = r;
                        }
                        return;
                    }

                    // Ldfun needs some special treatment sometimes:
                    // Since non closure bindings are skipped at runtime, we can
                    // only resolve ldfun if we are certain which one is the
                    // first binding that holds a closure. Often this is only
                    // possible after inlining a promise. But inlining a promise
                    // requires a force instruction. But ldfun does force
                    // implicitly. To get out of this vicious circle, we add the
                    // first binding we find with a normal load (as opposed to
                    // loadFun) from the abstract state as a "guess" This will
                    // enable other passes (especially the promise inliner pass)
                    // to work on the guess and maybe the next time we end up
                    // here, we can actually prove that the guess was right.
                    if (auto ldfun = LdFun::Cast(i)) {
                        auto guess = ldfun->guessedBinding();
                        // If we already have a guess, let's see if now know
                        // that it is a closure.
                        if (guess) {
                            // TODO: if !guess->maybe(closure) we know that the
                            // guess is wrong and could try the next binding.
                            if (!guess->type.isA(PirType::closure())) {
                                if (auto i = Instruction::Cast(guess)) {
                                    analysis.lookupAt(
                                        before, i,
                                        [&](const AbstractPirValue& res) {
                                            if (auto val =
                                                    getSingleLocalValue(res))
                                                guess = val;
                                        });
                                }
                            }
                            if (guess->type.isA(PirType::closure()) &&
                                guess->validIn(function)) {
                                guess = getReplacedValue(guess);
                                ldfun->replaceUsesWith(guess);
                                replacedValue[ldfun] = guess;
                                next = bb->remove(ip);
                                return;
                            }
                        } else {
                            auto res =
                                analysis
                                    .load(before, ldfun->varName, ldfun->env())
                                    .result;
                            if (auto firstBinding = getSingleLocalValue(res)) {
                                ip = bb->insert(
                                    ip, new Force(firstBinding, ldfun->env()));
                                ldfun->guessedBinding(*ip);
                                next = ip + 2;
                                return;
                            }
                        }
                    }

                    // If nothing else, narrow down the environment (in case we
                    // found something more concrete).
                    if (i->hasEnv() &&
                        aLoad.env != AbstractREnvironment::UnknownParent)
                        i->env(aLoad.env);
                });

                if (auto b = CallBuiltin::Cast(i)) {
                    bool noObjects = true;
                    i->eachArg([&](Value* v) {
                        if (v != i->env())
                            if (v->cFollowCastsAndForce()->type.maybeObj())
                                noObjects = false;
                    });

                    if (noObjects &&
                        SafeBuiltinsList::nonObject(b->builtinId)) {
                        std::vector<Value*> args;
                        i->eachArg([&](Value* v) {
                            if (v != i->env()) {
                                auto mk = MkArg::Cast(v);
                                if (mk && mk->isEager())
                                    args.push_back(mk->eagerArg());
                                else
                                    args.push_back(v);
                            }
                        });
                        auto safe =
                            new CallSafeBuiltin(b->blt, args, b->srcIdx);
                        b->replaceUsesWith(safe);
                        bb->replace(ip, safe);
                        replacedValue[b] = safe;
                    }
                }

                ip = next;
            }
        });
    }
};
} // namespace

namespace rir {
namespace pir {

void ScopeResolution::apply(RirCompiler&, ClosureVersion* function,
                            LogStream& log) const {
    TheScopeResolution s(function, log);
    s();
}

} // namespace pir
} // namespace rir
