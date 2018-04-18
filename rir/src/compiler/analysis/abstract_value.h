#ifndef PIR_ABSTRACT_VALUE_H
#define PIR_ABSTRACT_VALUE_H

#include "../pir/pir.h"

#include <functional>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace rir {
namespace pir {
struct ValOrig {
    Value* val;
    Instruction* origin;
    ValOrig(Value* v, Instruction* o) : val(v), origin(o) {}
    bool operator<(const ValOrig& other) const {
        if (origin == other.origin)
            return val < other.val;
        return origin < other.origin;
    }
    bool operator==(const ValOrig& other) const {
        return val == other.val && origin == other.origin;
    }
};
}
}

namespace std {
template <>
struct hash<rir::pir::ValOrig> {
    std::size_t operator()(const rir::pir::ValOrig& v) const {
        using std::hash;
        return hash<rir::pir::Value*>()(v.val) ^
               hash<rir::pir::Instruction*>()(v.origin);
    }
};
}

namespace rir {
namespace pir {

/*
 * Captures an abstract PIR value.
 *
 * Vals is the set of potential candidates. If we don't can't tell what the
 * possible values are, then we set "unknown" (ie. we taint the value). This is
 * the top element of our lattice.
 *
 */
struct AbstractPirValue {
  private:
    bool unknown = false;
    // This needs to be ordered set, for std::includes check!
    std::set<ValOrig> vals;

  public:
    PirType type = PirType::bottom();

    AbstractPirValue();
    AbstractPirValue(Value* v, Instruction* origin);

    static AbstractPirValue tainted() {
        AbstractPirValue v;
        v.taint();
        return v;
    }

    void taint() {
        vals.clear();
        unknown = true;
        type = PirType::any();
    }

    bool isUnknown() const { return unknown; }

    bool isSingleValue() {
        if (unknown)
            return false;
        return vals.size() == 1;
    }

    typedef std::function<void(Value*)> ValMaybe;
    typedef std::function<void(ValOrig&)> ValOrigMaybe;

    void ifSingleValue(ValMaybe known) {
        if (!unknown && vals.size() == 1)
            known((*vals.begin()).val);
    }

    void eachSource(ValOrigMaybe apply) {
        for (auto v : vals)
            apply(v);
    }

    bool merge(const AbstractPirValue& other);

    void print(std::ostream& out = std::cout);
};

/*
 * An AbstractREnvironment is a static approximation of an R runtime Envrionment
 *
 * A key notion is, when an environment leaks. A leaked environment describes
 * an environment, that is visible to an unknown context. This means, that it
 * can be inspected and manipulated by code we can't statically analyze.
 *
 * Typically an analysis will need to mark an environment leaked, when we call
 * a (statically) unknown function. The reason is that the callee can always
 * inspect our environment through sys.parent().
 *
 * For inter-procedural analysis we can additionally keep track of closures.
 */
class MkFunCls;
struct AbstractREnvironment {
    static Value* UnknownParent;
    static Value* UninitializedParent;
    static MkFunCls* UnknownClosure;

    std::unordered_map<SEXP, AbstractPirValue> entries;
    std::unordered_map<Value*, MkFunCls*> mkClosures;

    Value* parentEnv = UninitializedParent;

    bool leaked = false;
    bool tainted = false;

    void taint() {
        tainted = true;
        for (auto& e : entries) {
            e.second.taint();
        }
    }

    void set(SEXP n, Value* v, Instruction* origin) {
        entries[n] = AbstractPirValue(v, origin);
    }

    void print(std::ostream& out = std::cout) {
        for (auto e : entries) {
            SEXP name = std::get<0>(e);
            out << "   " << CHAR(PRINTNAME(name)) << " -> ";
            AbstractPirValue v = std::get<1>(e);
            v.print(out);
            out << "\n";
        }
        out << "\n";
    }

    const AbstractPirValue& get(SEXP e) const {
        static AbstractPirValue t = AbstractPirValue::tainted();
        if (entries.count(e))
            return entries.at(e);
        return t;
    }

    bool merge(const AbstractREnvironment& other) {
        bool changed = false;
        if (!leaked && other.leaked)
            changed = leaked = true;
        if (!tainted && other.tainted)
            changed = tainted = true;

        std::unordered_set<SEXP> keys;
        for (auto e : entries)
            keys.insert(std::get<0>(e));
        for (auto e : other.entries)
            keys.insert(std::get<0>(e));
        for (auto n : keys) {
            // if this is not the first incoming edge and it has more entries
            // we are in trouble.
            if (!entries.count(n)) {
                entries[n].taint();
                changed = true;
            } else if (entries[n].merge(other.get(n))) {
                changed = true;
            }
        }

        std::unordered_set<Value*> fps;
        for (auto e : mkClosures)
            fps.insert(std::get<0>(e));
        for (auto e : other.mkClosures)
            fps.insert(std::get<0>(e));
        for (auto n : fps) {
            if (mkClosures[n] != UnknownClosure &&
                (other.mkClosures.count(n) == 0 ||
                 mkClosures[n] != other.mkClosures.at(n))) {
                mkClosures[n] = UnknownClosure;
            }
        }

        if (parentEnv == UninitializedParent &&
            other.parentEnv != UninitializedParent) {
            parentEnv = other.parentEnv;
            changed = true;
        }

        return changed;
    }
};

/*
 * AbstractEnvironmentSet is an abstract domain that deals with multiple
 * environments at the same time. This is necessary for inter-procedural
 * analysis, or analyzing a function with multiple environments.
 *
 */

struct AbstractLoad {
    Value* env;
    AbstractPirValue result;

    AbstractLoad(Value* env, const AbstractPirValue& val)
        : env(env), result(val) {
        assert(env);
    }
};

class AbstractREnvironmentHierarchy
    : public std::unordered_map<Value*, AbstractREnvironment> {
  public:
    bool merge(const AbstractREnvironmentHierarchy& other) {
        bool changed = false;
        std::unordered_set<Value*> k;
        for (auto e : *this)
            k.insert(e.first);
        for (auto e : other)
            k.insert(e.first);
        for (auto i : k)
            if (this->count(i)) {
                if (other.count(i) == 0 && !at(i).tainted) {
                    at(i).taint();
                } else if (at(i).merge(other.at(i))) {
                    changed = true;
                }
            } else {
                (*this)[i].taint();
                changed = true;
            }
        return changed;
    }

    MkFunCls* findClosure(Value* env, Value* fun) {
        while (env && env != AbstractREnvironment::UnknownParent) {
            if ((*this)[env].mkClosures.count(fun))
                return (*this)[env].mkClosures.at(fun);
            env = (*this)[env].parentEnv;
        }
        return AbstractREnvironment::UnknownClosure;
    }

    AbstractLoad get(Value* env, SEXP e) const;
};
}
}

#endif
