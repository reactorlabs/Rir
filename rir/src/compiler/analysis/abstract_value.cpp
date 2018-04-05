#include "../pir/pir_impl.h"
#include "query.h"
#include "scope.h"

#include <algorithm>

namespace rir {
namespace pir {

AbstractPirValue::AbstractPirValue() : type(PirType::bottom()) {}
AbstractPirValue::AbstractPirValue(Value* v, Instruction* o) : type(v->type) {
    vals.insert(ValOrig(v, o));
}

bool AbstractPirValue::merge(const AbstractPirValue& other) {
    assert(other.type != PirType::bottom());

    if (unknown)
        return false;
    if (type == PirType::bottom()) {
        *this = other;
        return true;
    }

    bool changed = false;
    if (!std::includes(vals.begin(), vals.end(), other.vals.begin(),
                       other.vals.end())) {
        vals.insert(other.vals.begin(), other.vals.end());
        changed = true;
    }

    return changed;
}

void AbstractPirValue::print(std::ostream& out) {
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

AbstractLoad AbstractREnvironmentHierarchy::get(Value* env, SEXP e) const {
    while (env != AbstractREnvironment::UnknownParent) {
        if (this->count(env) == 0)
            return AbstractLoad(env, AbstractPirValue::tainted());
        auto aenv = this->at(env);
        const AbstractPirValue& res = aenv.get(e);
        if (!res.isUnknown())
            return AbstractLoad(env, res);
        // Tainted environment, we can't bet on absent values being
        // actually absent.
        if (aenv.tainted)
            return AbstractLoad(env, AbstractPirValue::tainted());
        env = at(env).parentEnv;
        if (env == AbstractREnvironment::UnknownParent && Env::parentEnv(env))
            env = Env::parentEnv(env);
    }
    return AbstractLoad(env, AbstractPirValue::tainted());
}

Value* AbstractREnvironment::UnknownParent = (Value*)-1;
Value* AbstractREnvironment::UninitializedParent = nullptr;
MkFunCls* AbstractREnvironment::UnknownClosure = (MkFunCls*)-1;
}
}
