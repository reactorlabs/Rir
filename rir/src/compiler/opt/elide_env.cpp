#include "elide_env.h"
#include "../pir/pir_impl.h"
#include "../util/cfg.h"
#include "../util/visitor.h"
#include "R/r.h"

#include <unordered_map>

namespace rir {
namespace pir {

void ElideEnv::apply(Function* function) {
    std::set<Value*> envNeeded;
    std::unordered_map<Value*, Value*> envDependency;

    Visitor::run(function->entry, [&](BB* bb) {
        for (auto i : *bb) {
            if (i->leaksEnv() || i->needsLiveEnv())
                envNeeded.insert(i->env());
            if (!Env::isEnv(i) && i->hasEnv())
                envDependency[i] = i->env();
        }
    });

    Visitor::run(function->entry, [&](BB* bb) {
        for (auto i : *bb) {
            if (i->mightIO() || i->type != PirType::voyd()) {
                i->each_arg([&](Value* v, PirType) {
                    if (envDependency.count(v))
                        envNeeded.insert(envDependency.at(v));
                });
            }
        }
    });

    Visitor::run(function->entry, [&](BB* bb) {
        auto ip = bb->begin();
        while (ip != bb->end()) {
            Instruction* i = *ip;
            if (Env::isEnv(i)) {
                if (envNeeded.find(i) == envNeeded.end())
                    ip = bb->remove(ip);
                else
                    ip++;
            } else if (i->hasEnv() &&
                       envNeeded.find(i->env()) == envNeeded.end()) {
                ip = bb->remove(ip);
            } else {
                ip++;
            }
        }
    });
}
}
}
