#include "../analysis/available_checkpoints.h"
#include "../pir/pir_impl.h"
#include "../transform/bb.h"
#include "../util/cfg.h"
#include "../util/visitor.h"
#include "R/r.h"
#include "pass_definitions.h"

#include <unordered_map>

namespace rir {
namespace pir {

void ElideEnvSpec::apply(RirCompiler&, ClosureVersion* function,
                         LogStream& log) const {

    AvailableCheckpoints checkpoint(function, log);

    auto nonObjectArgs = [&](Instruction* i) {
        auto answer = true;
        i->eachArg([&](Value* arg) {
            if (arg->type.maybeObj() && arg->typeFeedback.maybeObj())
                answer = false;
        });
        return answer;
    };

    Visitor::run(function->entry, [&](BB* bb) {
        auto ip = bb->begin();
        while (ip != bb->end()) {
            Instruction* i = *ip;
            auto next = ip + 1;

            if (i->hasEnv()) {
                // Speculatively elide environments on instructions in which
                // both operators are primitive values
                if (checkpoint.at(i) && i->envOnlyForObj() &&
                    nonObjectArgs(i)) {
                    i->elideEnv();
                    i->eachArg([&](Value* arg) {
                        if (arg != i->env())
                            if (arg->type.maybeObj()) {
                                auto condition =
                                    new TypeTest(arg, TypeTest::Object);
                                ip = bb->insert(ip, condition);
                                ip++;
                                ip = bb->insert(
                                    ip,
                                    (new Assume(condition, checkpoint.at(i)))
                                        ->Not());
                                ip++;
                            }
                    });
                    next = ip + 1;
                    i->type.setNotObject();
                }

                // Speculatively elide envs on forces that only require them in
                // case they access promises reflectively
                if (auto force = Force::Cast(i)) {
                    if (auto cp = checkpoint.next(i)) {
                        auto environment = MkEnv::Cast(force->env());
                        static std::unordered_set<Tag> forces{Tag::Force,
                                                              Tag::FrameState};

                        if (!environment->stub &&
                            environment->usesAreOnly(function->entry, forces)) {

                            environment = MkEnv::Cast(force->env());
                            environment->stub = true;
                            auto condition = new TypeTest(
                                environment, TypeTest::EnvironmentStub);
                            BBTransform::insertAssume(bb->trueBranch(),
                                                      condition, cp, true);
                        }
                    }
                }
            }
            ip = next;
        }
    });
}
} // namespace pir
} // namespace rir
