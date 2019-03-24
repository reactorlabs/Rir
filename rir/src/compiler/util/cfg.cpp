#include "cfg.h"
#include "../pir/pir_impl.h"
#include "../util/visitor.h"

#include <algorithm>
#include <set>
#include <stack>

namespace rir {
namespace pir {

CFG::CFG(Code* start)
    : predecessors_(start->nextBBId), transitivePredecessors(start->nextBBId) {
    Visitor::run(start->entry, [&](BB* bb) {
        auto apply = [&](BB* next) {
            if (!next)
                return;
            if (!isImmediatePredecessor(bb, next)) {
                predecessors_[next->id].push_back(bb);
                transitivePredecessors[next->id].push_back(bb);
            }
        };
        apply(bb->trueBranch());
        apply(bb->falseBranch());
        if (!bb->trueBranch() && !bb->falseBranch())
            exits_.push_back(bb);
    });

    std::function<void(BB*, BB*)> complete = [&](BB* bb, BB* pre1) {
        for (auto pre2 : immediatePredecessors(pre1)) {
            if (!isPredecessor(pre2, bb)) {
                transitivePredecessors[bb->id].push_back(pre2);
                complete(bb, pre2);
            }
        }
    };

    Visitor::run(start->entry, [&](BB* bb) {
        for (auto pre1 : immediatePredecessors(bb))
            complete(bb, pre1);
    });
}

bool CFG::isMergeBlock(BB* a) const { return predecessors_[a->id].size() > 1; }

bool CFG::hasSinglePred(BB* a) const {
    return predecessors_[a->id].size() == 1;
}

bool CFG::isPredecessor(BB* a, BB* b) const {
    auto& preds = transitivePredecessors[b->id];
    return std::any_of(
        preds.begin(), preds.end(),
        std::bind(std::equal_to<BB*>(), std::placeholders::_1, a));
}

bool CFG::isImmediatePredecessor(BB* a, BB* b) const {
    auto& preds = predecessors_[b->id];
    return std::any_of(
        preds.begin(), preds.end(),
        std::bind(std::equal_to<BB*>(), std::placeholders::_1, a));
}

const CFG::BBList& CFG::immediatePredecessors(BB* a) const {
    return predecessors_[a->id];
}

bool DominanceGraph::DomTree::merge(const DomTree& other) {
    bool changed = false;
    auto it = container.begin();
    while (it != container.end()) {
        if (std::find(other.container.begin(), other.container.end(), *it) ==
            other.container.end()) {
            it = container.erase(it);
            changed = true;
        } else {
            it++;
        }
    }
    return changed;
        }

DominanceGraph::DominanceGraph(Code* start) : dominating(start->nextBBId) {
    // Static Analysis computes the set of all dominating bb's, for every bb
    // reachable from start. Runs until none of the sets grow anymore.

    std::stack<BB*> todo;
    todo.push(start->entry);

    while (!todo.empty()) {
        BB* cur = todo.top();
        DomTree curState = dominating[cur->id];
        curState.push_back(cur);
        curState.seen = true;

        todo.pop();

        auto apply = [&](BB* bb) {
            if (!bb)
                return;

            auto& d = dominating[bb->id];
            if (!d.seen) {
                d = curState;
                todo.push(bb);
                return;
            }

            if (d.merge(curState))
                todo.push(bb);
        };
        apply(cur->trueBranch());
        apply(cur->falseBranch());
    }
}

bool DominanceGraph::dominates(BB* a, BB* b) const {
    const auto& doms = dominating[b->id].container;
    return std::find(doms.begin(), doms.end(), a) != doms.end();
}

bool DominanceGraph::immediatelyDominates(BB* a, BB* b) const {
    const auto& doms = dominating[b->id].container;
    if (doms.empty())
        return false;
    return doms[doms.size() - 1] == a;
}

BB* DominanceGraph::immediateDominator(BB* bb) const {
    const auto& doms = dominating[bb->id].container;
    return doms[doms.size() - 1];
}

const DominanceGraph::BBList& DominanceGraph::dominators(BB* bb) const {
    return dominating[bb->id].container;
}

void DominanceGraph::dominatorTreeNext(
    BB* bb, const std::function<void(BB*)>& apply) const {
    Visitor::run(bb, [&](BB* b) {
        if (immediatelyDominates(bb, b))
            apply(b);
    });
};

const DominanceFrontier::BBList& DominanceFrontier::at(BB* bb) const {
    return frontier[bb->id];
}

DominanceFrontier::DominanceFrontier(Code* code, const CFG& cfg,
                                     const DominanceGraph& dom) {
    frontier.resize(code->nextBBId);
    Visitor::run(code->entry, [&](BB* n) {
        for (const auto& p : cfg.immediatePredecessors(n)) {
            auto r = p;
            while (r != dom.immediateDominator(n)) {
                frontier[r->id].insert(n);
                r = dom.immediateDominator(r);
            }
        }
    });
}
}
}
