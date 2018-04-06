#ifndef COMPILER_VISITOR_H
#define COMPILER_VISITOR_H

#include "../pir/bb.h"
#include "../pir/pir.h"

#include <deque>
#include <functional>
#include <random>
#include <unordered_set>

namespace rir {
namespace pir {

namespace VisitorHelpers {
typedef std::function<bool(BB*)> BBActionPredicate;
typedef std::function<void(BB*)> BBAction;

/*
 * PredicateWrapper abstracts over BBAction and BBActionPredicate, to be
 * able to use them in the same generic implementation.
 * In the case of BBAction the return value will always be true.
 *
 */
template <typename ActionKind>
struct PredicateWrapper {};

template <>
struct PredicateWrapper<BBAction> {
    const BBAction action;
    bool operator()(BB* bb) const {
        action(bb);
        return true;
    }
};

template <>
struct PredicateWrapper<BBActionPredicate> {
    const BBActionPredicate action;
    bool operator()(BB* bb) const { return action(bb); }
};

/*
 * Helpers to remember which BB has already been visited. There is a fast
 * version (IDMarker) that uses a bitvector, but relies on stable BB ids. And
 * there is a slow version (PointerMarker) using a set.
 *
 */
struct IDMarker {
    std::vector<bool> done;
    IDMarker() : done(128, false){};

    void set(BB* bb) {
        while (bb->id >= done.size())
            done.resize(done.size() * 2);
        done[bb->id] = true;
    }

    bool check(BB* bb) { return bb->id < done.size() && done[bb->id]; }
};

struct PointerMarker {
    std::unordered_set<BB*> done;
    void set(BB* bb) { done.insert(bb); }
    bool check(BB* bb) { return done.find(bb) != done.end(); }
};
};

template <bool STABLE, class Marker>
class VisitorImplementation {
  public:
    typedef std::function<bool(Instruction*)> InstrActionPredicate;
    typedef std::function<void(Instruction*)> InstrAction;
    typedef std::function<bool(Instruction*, BB*)> InstrBBActionPredicate;
    typedef std::function<void(Instruction*, BB*)> InstrBBAction;
    using BBActionPredicate = VisitorHelpers::BBActionPredicate;
    using BBAction = VisitorHelpers::BBAction;

    /*
     * Instruction Visitors
     *
     */
    static void run(BB* bb, InstrAction action) {
        run(bb, [action](BB* bb) {
            for (auto i : *bb)
                action(i);
        });
    }

    static bool check(BB* bb, InstrActionPredicate action) {
        return check(bb, [action](BB* bb) {
            bool holds = true;
            for (auto i : *bb) {
                if (!action(i)) {
                    holds = false;
                    break;
                }
            }
            return holds;
        });
    }

    static void run(BB* bb, InstrBBAction action) {
        run(bb, [action](BB* bb) {
            for (auto i : *bb)
                action(i, bb);
        });
    }

    static bool check(BB* bb, InstrBBActionPredicate action) {
        return check(bb, [action](BB* bb) {
            bool holds = true;
            for (auto i : *bb) {
                if (!action(i, bb)) {
                    holds = false;
                    break;
                }
            }
            return holds;
        });
    }

    /*
     * BB Visitors
     *
     */
    static void run(BB* bb, BBAction action) { genericRun(bb, action); }

    static bool check(BB* bb, BBActionPredicate action) {
        return genericRun(bb, action);
    }

    template <typename ActionKind>
    static bool genericRun(BB* bb, ActionKind action) {
        typedef VisitorHelpers::PredicateWrapper<ActionKind> PredicateWrapper;
        const PredicateWrapper predicate = {action};

        BB* cur = bb;
        std::deque<BB*> todo;
        Marker done;
        done.set(cur);

        while (cur) {
            BB* next = nullptr;

            if (cur->next0 && !done.check(cur->next0)) {
                if (todo.empty())
                    next = cur->next0;
                else
                    enqueue(todo, cur->next0);
                done.set(cur->next0);
            }

            if (cur->next1 && !done.check(cur->next1)) {
                if (!next && todo.empty()) {
                    next = cur->next1;
                } else {
                    enqueue(todo, cur->next1);
                }
                done.set(cur->next1);
            }

            if (!next) {
                if (!todo.empty()) {
                    next = todo.front();
                    todo.pop_front();
                }
            }

            if (!predicate(cur))
                return false;

            cur = next;
        }
        assert(todo.empty());

        return true;
    }

  private:
    static bool coinFlip() {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::bernoulli_distribution coin(0.5);
        return coin(gen);
    };

    static void enqueue(std::deque<BB*>& todo, BB* bb) {
        // For analysis random search is faster
        if (STABLE || coinFlip())
            todo.push_back(bb);
        else
            todo.push_front(bb);
    }
};

class Visitor
    : public VisitorImplementation<false, VisitorHelpers::IDMarker> {};
class BreadthFirstVisitor
    : public VisitorImplementation<true, VisitorHelpers::IDMarker> {};
class UnstableIDsVisitor
    : public VisitorImplementation<true, VisitorHelpers::PointerMarker> {};
}
}

#endif
