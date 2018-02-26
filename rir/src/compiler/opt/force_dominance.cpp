#include "force_dominance.h"
#include "../analysis/generic_static_analysis.h"
#include "../pir/pir_impl.h"
#include "../transform/bb.h"
#include "../transform/replace.h"

namespace {

using namespace rir::pir;

/* This optimization removes redundant force instructions:
 *
 * b = force(a)
 * c = force(b)
 *
 * For that we need to compute a dominance graph of forces.
 *
 * Additionally, if know the promise being forced, we try to inline it. For
 * example:
 *
 * a = mkArg(prom(0))
 * b = force(a)
 *
 * will be translated to:
 *
 * b = <inlined prom(0)>
 *
 * But, in the case of promises with side-effects we can only inline them iff
 * there is a unique dominating force instruction.
 *
 * For example in the following case:
 *
 *      Branch
 *   /          \
 * force         |
 *   \         /
 *     \     /
 *        |
 *      force
 *
 * we don't know at the second force if the promise was forced (by the left
 * branch) or not. Thus we cannot inline it.
 */

struct ForcedAt : public std::unordered_map<Value*, Force*> {
    static Force* ambiguous() { return (Force*)0x22; }

    void evalAt(Value* val, Force* force) {
        if (!count(val))
            (*this)[val] = force;
    }
    bool merge(ForcedAt& other) {
        bool changed = false;
        for (auto& e : *this) {
            auto v = e.first;
            auto f = e.second;
            if (f != ambiguous() && (!other.count(v) || f != other.at(v))) {
                e.second = ambiguous();
                changed = true;
            }
        }
        for (auto& e : other) {
            if (!count(e.first)) {
                (*this)[e.first] = ambiguous();
                changed = true;
            }
        }
        return changed;
    }
};

static Value* getValue(Force* f) {
    Value* res;
    while (f) {
        res = f->arg<0>();
        f = Force::Cast(res);
    }
    return res;
}

class ForceDominanceAnalysis : public StaticAnalysis<ForcedAt> {
  public:
    ForceDominanceAnalysis(BB* bb) : StaticAnalysis(bb) {}
    std::unordered_map<Force*, Force*> domBy;
    std::set<Force*> dom;

    void apply(ForcedAt& d, Instruction* i) const override {
        auto f = Force::Cast(i);
        if (f)
            d.evalAt(getValue(f), f);
    }

    void operator()() {
        StaticAnalysis::operator()();

        collect<false>([&](const ForcedAt& p, Instruction* i) {
            auto f = Force::Cast(i);
            if (f) {
                if (p.find(getValue(f)) != p.end()) {
                    auto o = p.at(getValue(f));
                    if (o != ForcedAt::ambiguous()) {
                        if (f != o)
                            domBy[f] = o;
                        else
                            dom.insert(f);
                    }
                }
            }
        });
    }
    bool safeToInline(MkArg* a) {
        return exitpoint.count(a) && exitpoint.at(a) != ForcedAt::ambiguous();
    }
    bool isDominating(Force* f) { return dom.find(f) != dom.end(); }
    void map(Force* f, std::function<void(Force* f)> action) {
        if (domBy.count(f))
            action(domBy.at(f));
    }
};
}

namespace rir {
namespace pir {

void ForceDominance::apply(Function* function) {
    ForceDominanceAnalysis dom(function->entry);
    dom();

    std::unordered_map<Force*, Value*> inlinedPromise;

    Visitor::run(function->entry, [&](BB* bb) {
        auto ip = bb->begin();
        while (ip != bb->end()) {
            auto f = Force::Cast(*ip);
            auto next = ip + 1;
            if (f) {
                auto mkarg = MkArg::Cast(getValue(f));
                if (mkarg) {
                    if (dom.isDominating(f)) {
                        Value* strict = mkarg->arg<0>();
                        if (strict != Missing::instance()) {
                            f->replaceUsesWith(strict);
                            next = bb->remove(ip);
                            inlinedPromise[f] = strict;
                        } else if (dom.safeToInline(mkarg)) {
                            Promise* prom = mkarg->prom;
                            BB* split = BBTransform::split(
                                ++function->max_bb_id, bb, ip, function);
                            BB* prom_copy = BBTransform::clone(
                                &function->max_bb_id, prom->entry, function);
                            bb->next0 = prom_copy;

                            // For now we assume every promise starts with a
                            // LdFunctionEnv instruction. We replace it's usages
                            // with the caller environment.
                            LdFunctionEnv* e =
                                LdFunctionEnv::Cast(*prom_copy->begin());
                            assert(e);
                            Replace::usesOfValue(prom_copy, e, mkarg->env());
                            prom_copy->remove(prom_copy->begin());

                            // Create a return value phi of the promise
                            Value* promRes =
                                BBTransform::forInline(prom_copy, split);

                            f = Force::Cast(*split->begin());
                            assert(f);
                            f->replaceUsesWith(promRes);
                            next = split->remove(split->begin());
                            bb = split;

                            inlinedPromise[f] = promRes;
                        }
                    }
                }
            }
            ip = next;
        }
    });

    Visitor::run(function->entry, [&](BB* bb) {
        auto ip = bb->begin();
        while (ip != bb->end()) {
            auto f = Force::Cast(*ip);
            auto next = ip + 1;
            if (f) {
                // If this force instruction is dominated by another force we
                // can replace it with the dominating instruction
                dom.map(f, [&](Force* r) {
                    if (inlinedPromise.count(r))
                        f->replaceUsesWith(inlinedPromise.at(r));
                    else
                        f->replaceUsesWith(r);
                    next = bb->remove(ip);
                });
            }
            ip = next;
        }
    });
}
}
}
