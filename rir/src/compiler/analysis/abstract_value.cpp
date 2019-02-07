#include "../pir/pir_impl.h"
#include "query.h"
#include "scope.h"

#include <algorithm>

namespace rir {
namespace pir {

AbstractPirValue::AbstractPirValue() : type(PirType::bottom()) {}
AbstractPirValue::AbstractPirValue(Value* v, Instruction* o,
                                   unsigned recursionLevel)
    : type(v->type) {
    vals.insert(ValOrig(v, o, recursionLevel));
}

void AbstractREnvironmentHierarchy::print(std::ostream& out, bool tty) const {
    for (auto& e : envs) {
        out << "== [";
        e.first->printRef(out);
        out << "]\n";
        e.second.print(out);
    }
    for (auto& a : aliases) {
        out << "* ";
        a.first->printRef(out);
        out << " = ";
        a.second->printRef(out);
        out << "\n";
    }
}

void AbstractREnvironment::print(std::ostream& out, bool tty) const {
    if (leaked)
        out << "* leaked\n";
    if (tainted)
        out << "* tainted\n";

    for (auto e : entries) {
        SEXP name = std::get<0>(e);
        out << "   " << CHAR(PRINTNAME(name)) << " -> ";
        AbstractPirValue v = std::get<1>(e);
        v.print(out);
        out << "\n";
    }
}

AbstractResult AbstractPirValue::merge(const AbstractPirValue& other) {
    assert(other.type != PirType::bottom());

    if (unknown)
        return AbstractResult::None;
    if (type == PirType::bottom()) {
        *this = other;
        return AbstractResult::Updated;
    }
    if (other.unknown) {
        taint();
        return AbstractResult::LostPrecision;
    }

    bool changed = false;
    if (!std::includes(vals.begin(), vals.end(), other.vals.begin(),
                       other.vals.end())) {
        vals.insert(other.vals.begin(), other.vals.end());
        changed = true;
    }
    changed = type.merge(other.type) || changed;

    return changed ? AbstractResult::Updated : AbstractResult::None;
}

void AbstractPirValue::print(std::ostream& out, bool tty) const {
    if (unknown) {
        out << "??";
        return;
    }
    out << "(";
    for (auto it = vals.begin(); it != vals.end();) {
        auto vo = *it;
        vo.val->printRef(out);
        out << "@";
        vo.origin->printRef(out);
        it++;
        if (it != vals.end())
            out << "|";
    }
    out << ") : " << type;
}

std::unordered_set<Value*>
AbstractREnvironmentHierarchy::potentialParents(Value* env) const {
    std::unordered_set<Value*> res;
    assert(env);
    if (aliases.count(env))
        env = aliases.at(env);
    while (envs.count(env)) {
        res.insert(env);
        auto aenv = envs.at(env);
        auto parent = envs.at(env).parentEnv();
        assert(parent);
        if (parent == AbstractREnvironment::UnknownParent &&
            Env::parentEnv(env))
            env = Env::parentEnv(env);
        else
            env = parent;
        if (env == Env::nil())
            return res;
    }
    // We did not reach the outer most environment of the current closure.
    // Therefore we have no clue which envs are the actual parents. The
    // conservative choice is to return all candidates.
    for (auto e : envs)
        res.insert(e.first);
    return res;
}

AbstractLoad AbstractREnvironmentHierarchy::get(Value* env, SEXP e) const {
    assert(env);
    if (aliases.count(env))
        env = aliases.at(env);
    while (env != AbstractREnvironment::UnknownParent) {
        assert(env);
        if (envs.count(env) == 0)
            return AbstractLoad(env, AbstractPirValue::tainted());
        auto aenv = envs.at(env);
        if (!aenv.absent(e)) {
            const AbstractPirValue& res = aenv.get(e);
            return AbstractLoad(env, res);
        }
        auto parent = envs.at(env).parentEnv();
        assert(parent);
        if (parent == AbstractREnvironment::UnknownParent &&
            Env::parentEnv(env))
            env = Env::parentEnv(env);
        else
            env = parent;
    }
    return AbstractLoad(env, AbstractPirValue::tainted());
}

// Looking up functions is slightly trickier, since non-function bindings have
// to be skipped.
AbstractLoad AbstractREnvironmentHierarchy::getFun(Value* env, SEXP e) const {
    assert(env);
    if (aliases.count(env))
        env = aliases.at(env);
    while (env != AbstractREnvironment::UnknownParent) {
        assert(env);
        if (envs.count(env) == 0)
            return AbstractLoad(env, AbstractPirValue::tainted());
        auto aenv = envs.at(env);
        if (!aenv.absent(e)) {
            const AbstractPirValue& res = aenv.get(e);

            // If it is a closure, we know we are good
            if (res.type.isA(RType::closure))
                return AbstractLoad(env, res);

            // If it might be a closure, we can neither be sure, nor exclude
            // this binding...
            if (res.type.maybe(RType::closure))
                return AbstractLoad(env, AbstractPirValue::tainted());
        }
        auto parent = envs.at(env).parentEnv();
        assert(parent);
        // If the analysis does not know what the parent env is, but the env is
        // an existing R env, we can get the parent from the actual R env object
        if (parent == AbstractREnvironment::UnknownParent &&
            Env::parentEnv(env))
            env = Env::parentEnv(env);
        else
            env = parent;
    }
    return AbstractLoad(env, AbstractPirValue::tainted());
}

AbstractLoad AbstractREnvironmentHierarchy::superGet(Value* env, SEXP e) const {
    if (aliases.count(env))
        env = aliases.at(env);
    if (!envs.count(env))
        return AbstractLoad(AbstractREnvironment::UnknownParent,
                            AbstractPirValue::tainted());
    auto parent = envs.at(env).parentEnv();
    assert(parent);
    if (parent == AbstractREnvironment::UnknownParent && Env::parentEnv(env))
        parent = Env::parentEnv(env);
    return get(parent, e);
}

Value* AbstractREnvironment::UnknownParent = (Value*)-1;
Value* AbstractREnvironment::UninitializedParent = (Value*)-2;
}
}
