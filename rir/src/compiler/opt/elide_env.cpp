#include "../pir/pir_impl.h"
#include "../util/cfg.h"
#include "../util/visitor.h"
#include "R/r.h"
#include "pass_definitions.h"

#include <unordered_map>

namespace rir {
namespace pir {

void ElideEnv::apply(Closure* function) const {
    std::unordered_set<Value*> envNeeded;
    std::unordered_map<Value*, Value*> envDependency;

    Visitor::run(function->entry, [&](Instruction* i) {
        if (i->hasEnv()) {
            if (!StVar::Cast(i))
                envNeeded.insert(i->env());
            if (!Env::isPirEnv(i))
                envDependency[i] = i->env();
        }
    });

    Visitor::run(function->entry, [&](Instruction* i) {
        if (i->hasEffect() || i->type != PirType::voyd() || Return::Cast(i) ||
            Deopt::Cast(i)) {
            i->eachArg([&](Value* v) {
                if (envDependency.count(v))
                    envNeeded.insert(envDependency.at(v));
            });
        }
    });

    Visitor::run(function->entry, [&](BB* bb) {
        auto ip = bb->begin();
        while (ip != bb->end()) {
            Instruction* i = *ip;
            if (Env::isPirEnv(i)) {
                if (envNeeded.find(i) == envNeeded.end())
                    ip = bb->remove(ip);
                else
                    ip++;
            } else if (i->hasEnv() && Env::isPirEnv(i->env()) &&
                       envNeeded.find(i->env()) == envNeeded.end()) {
                ip = bb->remove(ip);
            } else {
                ip++;
            }
        }
    });
}
} // namespace pir
} // namespace rir
