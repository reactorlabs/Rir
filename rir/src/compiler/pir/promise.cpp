#include "promise.h"
#include "compiler/pir/bb.h"
#include "compiler/pir/instruction.h"
#include "compiler/util/visitor.h"
#include "interpreter/instance.h"
#include "ir/BC.h"

namespace rir {
namespace pir {

Promise::Promise(ClosureVersion* owner, unsigned id, rir::Code* rirSrc)
    : id(id), owner(owner), rirSrc_(rirSrc), ast_(0) {}

Promise::Promise(ClosureVersion* owner, unsigned id, unsigned ast)
    : id(id), owner(owner), rirSrc_(nullptr), ast_(ast) {}

LdFunctionEnv* Promise::env() const {
    LdFunctionEnv* e = nullptr;
    Visitor::run(entry, [&](Instruction* i) {
        if (auto ld = LdFunctionEnv::Cast(i)) {
            assert(!e);
            e = ld;
        }
    });
    return e;
}

bool Promise::trivial() const {
    auto bb = entry;
    if (bb->isEmpty())
        bb = bb->next();
    for (auto i : *bb) {
        switch (i->tag) {
        case Tag::LdConst:
        case Tag::Visible:
        case Tag::Return:
            break;
        default:
            return false;
        }
    }
    return true;
}

} // namespace pir
} // namespace rir
