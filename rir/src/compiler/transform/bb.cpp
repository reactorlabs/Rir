#include "bb.h"
#include "../pir/pir_impl.h"
#include "../util/visitor.h"
#include "R/Funtab.h"
#include "R/r.h"

#include <unordered_map>

namespace rir {
namespace pir {

BB* BBTransform::clone(BB* src, Code* target, ClosureVersion* targetClosure) {
    std::vector<BB*> bbs;

    // Copy instructions and remember old -> new instruction map.
    std::unordered_map<Value*, Instruction*> relocation_table;
    Visitor::run(src, [&](BB* bb) {
        BB* theClone = BB::cloneInstrs(bb, target->nextBBId++, target);
        assert(bb->size() == theClone->size());
        if (bb->id >= bbs.size())
            bbs.resize(bb->id + 5);
        bbs[bb->id] = theClone;
        for (size_t i = 0; i < bb->size(); ++i)
            relocation_table[bb->at(i)] = theClone->at(i);
    });

    // Fixup CFG: next pointers of copied BB's need to be filled in.
    Visitor::run(src, [&](BB* bb) {
        bbs[bb->id]->next0 = bbs[bb->id]->next1 = nullptr;
        if (bb->next0)
            bbs[bb->id]->next0 = bbs[bb->next0->id];
        if (bb->next1)
            bbs[bb->id]->next1 = bbs[bb->next1->id];
    });

    std::unordered_map<Promise*, Promise*> promMap;
    // Relocate argument pointers using old -> new map
    BB* newEntry = bbs[src->id];
    Visitor::run(newEntry, [&](Instruction* i) {
        auto phi = Phi::Cast(i);
        if (phi) {
            for (size_t j = 0; j < phi->nargs(); ++j)
                phi->updateInputAt(j, bbs[phi->inputAt(j)->id]);
        }
        i->eachArg([&](InstrArg& arg) {
            if (arg.val()->isInstruction()) {
                assert(relocation_table.count(arg.val()));
                arg.val() = relocation_table.at(arg.val());
            }
        });
        if (auto mk = MkArg::Cast(i)) {
            Promise* p = mk->prom();
            if (p->owner != targetClosure) {
                if (promMap.count(p)) {
                    mk->updatePromise(promMap.at(p));
                } else {
                    auto c = targetClosure->createProm(p->srcPoolIdx());
                    c->entry = clone(p->entry, c, targetClosure);
                    mk->updatePromise(c);
                }
            }
        }
    });

    return newEntry;
}

BB* BBTransform::splitEdge(size_t next_id, BB* from, BB* to, Code* target) {
    BB* split = new BB(target, next_id);

    split->next0 = to;
    split->next1 = nullptr;

    if (from->next0 == to)
        from->next0 = split;
    else
        from->next1 = split;

    Visitor::run(split, [&](Instruction* i) {
        if (auto phi = Phi::Cast(i)) {
            for (size_t j = 0; j < phi->nargs(); ++j)
                if (phi->inputAt(j) == from)
                    phi->updateInputAt(j, split);
        }
    });

    return split;
}

BB* BBTransform::split(size_t next_id, BB* src, BB::Instrs::iterator it,
                       Code* target) {
    BB* split = new BB(target, next_id);
    split->next0 = src->next0;
    split->next1 = src->next1;
    while (it != src->end()) {
        it = src->moveToEnd(it, split);
    }
    src->next0 = split;
    src->next1 = nullptr;
    Visitor::run(split, [&](Instruction* i) {
        if (auto phi = Phi::Cast(i)) {
            for (size_t j = 0; j < phi->nargs(); ++j)
                if (phi->inputAt(j) == src)
                    phi->updateInputAt(j, split);
        }
    });
    return split;
}

void BBTransform::splitCriticalEdges(Code* fun) {
    std::vector<std::pair<BB*, BB*>> edges;
    CFG cfg(fun);

    // pred->bb is a critical edge if pred has multiple successors and bb
    // has multiple predecessors
    Visitor::run(fun->entry, [&](BB* bb) {
        if (cfg.isMergeBlock(bb)) {
            for (const auto& pred : cfg.immediatePredecessors(bb)) {
                if (pred->isBranch()) {
                    // Don't split edges while iterating over the CFG!
                    edges.emplace_back(std::make_pair(pred, bb));
                }
            }
        }
    });

    for (const auto& e : edges) {
        BBTransform::splitEdge(fun->nextBBId++, e.first, e.second, fun);
    }
}

std::pair<Value*, BB*> BBTransform::forInline(BB* inlinee, BB* splice) {
    Value* found = nullptr;
    Return* ret;
    Visitor::run(inlinee, [&](BB* bb) {
        if (bb->next0 != nullptr)
            return;

        assert(bb->next1 == nullptr);
        if (bb->isDeopt())
            return;

        ret = Return::Cast(bb->last());
        assert(ret);

        // This transformation assumes that we have just one reachable return.
        // Assert that we do not find a second one.
        assert(!found);

        found = ret->arg<0>().val();
        bb->next0 = splice;
        bb->remove(bb->end() - 1);
    });
    assert(found);
    return {found, ret->bb()};
}

BB* BBTransform::lowerExpect(Code* code, BB* src, BB::Instrs::iterator position,
                             Value* condition, bool expected, BB* deoptBlock,
                             const std::string& debugMessage) {
    auto split = BBTransform::split(code->nextBBId++, src, position + 1, code);

    static SEXP print = Rf_findFun(Rf_install("cat"), R_GlobalEnv);

    if (debugMessage.size() != 0) {
        BB* debug = new BB(code, code->nextBBId++);
        SEXP msg = Rf_mkString(debugMessage.c_str());
        auto ldprint = new LdConst(print);
        auto ldmsg = new LdConst(msg);
        debug->append(ldmsg);
        debug->append(ldprint);
        debug->append(new Call(Env::elided(), ldprint, {ldmsg},
                               Tombstone::framestate(), 0));
        debug->setNext(deoptBlock);
        deoptBlock = debug;
    }

    src->replace(position, new Branch(condition));
    if (expected) {
        src->next1 = deoptBlock;
        src->next0 = split;
    } else {
        src->next0 = deoptBlock;
        src->next1 = split;
    }

    splitEdge(code->nextBBId++, src, deoptBlock, code);

    return split;
}

void BBTransform::insertAssume(Value* condition, Checkpoint* cp, BB* bb,
                               BB::Instrs::iterator& position,
                               bool assumePositive) {
    position = bb->insert(position, (Instruction*)condition);
    auto assume = new Assume(condition, cp);
    if (!assumePositive)
        assume->Not();
    position = bb->insert(position + 1, assume);
    position++;
};

void BBTransform::insertAssume(Value* condition, Checkpoint* cp,
                               bool assumePositive) {
    auto contBB = cp->bb()->trueBranch();
    auto contBegin = contBB->begin();
    insertAssume(condition, cp, contBB, contBegin, assumePositive);
}

void BBTransform::renumber(Code* fun) {
    DominanceGraph dom(fun);
    fun->nextBBId = 0;
    DominatorTreeVisitor<VisitorHelpers::PointerMarker>(dom).run(
        fun->entry, [&](BB* bb) {
            bb->unsafeSetId(fun->nextBBId++);
            bb->gc();
        });
}

void BBTransform::removeDeadInstrs(Code* fun) {
    Visitor::run(fun->entry, [&](BB* bb) {
        auto ip = bb->begin();
        while (ip != bb->end()) {
            Instruction* i = *ip;
            auto next = ip + 1;
            if (i->unused() && !i->branchOrExit() &&
                !i->hasObservableEffects()) {
                next = bb->remove(ip);
            }
            ip = next;
        }
    });
}

} // namespace pir
} // namespace rir
