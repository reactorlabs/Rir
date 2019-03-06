#ifndef PIR_ABSTRACT_VALUE_H
#define PIR_ABSTRACT_VALUE_H

#include "../pir/pir.h"
#include "abstract_result.h"

#include <functional>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace rir {
namespace pir {

struct ValOrig {
    Value* val;
    Instruction* origin;
    unsigned recursionLevel;

    ValOrig(Value* v, Instruction* o, unsigned recursionLevel)
        : val(v), origin(o), recursionLevel(recursionLevel) {}

    bool operator<(const ValOrig& other) const {
        if (origin == other.origin && recursionLevel == other.recursionLevel)
            return val < other.val;
        if (origin == other.origin)
            return recursionLevel < other.recursionLevel;
        return origin < other.origin;
    }
    bool operator==(const ValOrig& other) const {
        return val == other.val && origin == other.origin &&
               recursionLevel == other.recursionLevel;
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

    AbstractPirValue(Value* v, Instruction* origin, unsigned recursionLevel);

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

    bool isSingleValue() const {
        if (unknown)
            return false;
        return vals.size() == 1;
    }

    const ValOrig& singleValue() const {
        assert(vals.size() == 1);
        return *vals.begin();
    }

    typedef std::function<void(Value*)> ValMaybe;
    typedef std::function<void(const ValOrig&)> ValOrigMaybe;
    typedef std::function<bool(const ValOrig&)> ValOrigMaybePredicate;

    void ifSingleValue(ValMaybe known) {
        if (!unknown && vals.size() == 1)
            known((*vals.begin()).val);
    }

    void eachSource(const ValOrigMaybe& apply) const {
        for (auto& v : vals)
            apply(v);
    }

    bool checkEachSource(const ValOrigMaybePredicate& apply) const {
        for (auto& v : vals)
            if (!apply(v))
                return false;
        return true;
    }

    AbstractResult merge(const ValOrig& other) {
        return merge(
            AbstractPirValue(other.val, other.origin, other.recursionLevel));
    }

    AbstractResult merge(const AbstractPirValue& other);

    void print(std::ostream& out, bool tty = false) const;
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
struct AbstractREnvironment {
    static Value* UnknownParent;
    static Value* UninitializedParent;

    std::unordered_map<SEXP, AbstractPirValue> entries;

    AbstractREnvironment() {}

    bool leaked = false;
    bool tainted = false;

    void taint() {
        tainted = true;
        for (auto& e : entries) {
            e.second.taint();
        }
    }

    void set(SEXP n, Value* v, Instruction* origin, unsigned recursionLevel) {
        entries[n] = AbstractPirValue(v, origin, recursionLevel);
    }

    void print(std::ostream& out, bool tty = false) const;

    const AbstractPirValue& get(SEXP e) const {
        static AbstractPirValue t = AbstractPirValue::tainted();
        if (entries.count(e))
            return entries.at(e);
        return t;
    }

    const bool absent(SEXP e) const { return !tainted && !entries.count(e); }

    AbstractResult merge(const AbstractREnvironment& other) {
        AbstractResult res;

        if (!leaked && other.leaked) {
            leaked = true;
            res.lostPrecision();
        }
        if (!tainted && other.tainted) {
            tainted = true;
            res.taint();
        }

        for (auto& entry : other.entries) {
            auto name = entry.first;
            if (!entries.count(name)) {
                entries[name].taint();
                res.lostPrecision();
            } else {
                res.max(entries[name].merge(other.get(name)));
            }
        }
        for (auto& entry : entries) {
            auto name = entry.first;
            if (!other.entries.count(name) && !entries.at(name).isUnknown()) {
                entries.at(name).taint();
                res.lostPrecision();
            }
        }

        if (parentEnv_ == UninitializedParent &&
            other.parentEnv_ != UninitializedParent) {
            parentEnv(other.parentEnv());
            res.update();
        } else if (parentEnv_ != UninitializedParent &&
                   parentEnv_ != UnknownParent &&
                   other.parentEnv_ != parentEnv_) {
            parentEnv_ = UnknownParent;
            res.lostPrecision();
        }

        return res;
    }

    Value* parentEnv() const {
        if (parentEnv_ == UninitializedParent)
            return UnknownParent;
        return parentEnv_;
    }

    void parentEnv(Value* v) {
        assert(v);
        parentEnv_ = v;
    }

  private:
    Value* parentEnv_ = UninitializedParent;
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

    explicit AbstractLoad(const AbstractPirValue& val)
        : env(AbstractREnvironment::UnknownParent), result(val) {}

    AbstractLoad(Value* env, const AbstractPirValue& val)
        : env(env), result(val) {
        assert(env);
    }
};

class AbstractREnvironmentHierarchy {
  private:
    std::unordered_map<Value*, AbstractREnvironment> envs;

  public:
    AbstractREnvironmentHierarchy() {}

    std::unordered_map<Value*, Value*> aliases;

    AbstractResult merge(const AbstractREnvironmentHierarchy& other) {
        AbstractResult res;

        for (auto& e : other.envs)
            if (envs.count(e.first))
                res.max(envs.at(e.first).merge(e.second));
            else
                envs.emplace(e);

        for (auto& entry : other.aliases) {
            if (!aliases.count(entry.first)) {
                aliases.emplace(entry);
                res.update();
            } else {
                SLOWASSERT(entry.second == aliases.at(entry.first));
            }
        }
        return res;
    }

    bool known(Value* env) const { return envs.count(env); }

    const AbstractREnvironment& at(Value* env) const {
        if (aliases.count(env))
            return envs.at(aliases.at(env));
        else
            return envs.at(env);
    }

    AbstractREnvironment& operator[](Value* env) {
        if (aliases.count(env))
            return envs[aliases.at(env)];
        else
            return envs[env];
    }

    void print(std::ostream& out, bool tty = false) const;

    AbstractLoad get(Value* env, SEXP e) const;
    AbstractLoad getFun(Value* env, SEXP e) const;
    AbstractLoad superGet(Value* env, SEXP e) const;

    std::unordered_set<Value*> potentialParents(Value* env) const;
};

template <typename Kind>
class AbstractUnique {
    Kind* val = nullptr;

  public:
    AbstractUnique() {}

    void set(Kind* val_) {
        assert(val_);
        val = val_;
    }

    void clear() {
        val = nullptr;
    }

    Kind* get() const { return val; }

    AbstractResult merge(const AbstractUnique& other) {
        if (val && val != other.val) {
            val = nullptr;
            return AbstractResult::Updated;
        }
        return AbstractResult::None;
    }

    void print(std::ostream& out, bool tty) const {
        if (val)
            val->printRef(out);
        else
            out << "?";
        out << "\n";
    };
};
}
}

#endif
