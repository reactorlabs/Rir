#ifndef COMPILER_TYPE_H
#define COMPILER_TYPE_H

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

#include "R/r_incl.h"
#include "ir/RuntimeFeedback.h"
#include "utils/EnumSet.h"

namespace rir {
namespace pir {

/*
 * Values in PIR are either types from R (RType), or native types (NativeType).
 *
 * In both cases we use union types, represented by a bitset.
 *
 * There is an additional flags bitset, that adds some modifiers.
 *
 * As an example, an R integer, that is potentially promised, has:
 *
 *  - flags_ : TypeFlag::rtype | TypeFlag::lazy
 *  - t_.r   : RType::integer
 *
 * A machine boolean has type:
 *
 *  - flags_ : ()
 *  - t_.n   : NativeType::test
 *
 * An R value (not a promise), has:
 *
 *  - flags_ : TypeFlag::rtype
 *  - t_.r   : RType::symbol | ... | RType::ast
 *
 */

enum class RType : uint8_t {
    _UNUSED_,

    nil,
    cons,

    sym,
    chr,

    logical,
    integer,
    real,
    str,
    vec,
    cplx,

    raw,

    closure,
    prom,

    missing,
    unbound,

    code,
    env,
    ast,

    FIRST = nil,
    LAST = ast
};

enum class NativeType : uint8_t {
    _UNUSED_,

    test,
    checkpoint,
    frameState,
    context,

    FIRST = test,
    LAST = frameState,
};

enum class TypeFlags : uint8_t {
    _UNUSED_,

    lazy,
    promiseWrapped,
    maybeNotScalar,
    maybeObject,
    rtype,

    FIRST = lazy,
    LAST = rtype
};

/*
 * A PirType can either represent a union of R types or of native types.
 *
 * `a :> b` is implemented by `a.isSuper(b)`. The primitive types are enumerated
 * by RType and NativeType respectively.
 *
 * TypeFlags are additional features. The element `rtype` of the type flags is
 * abused to store, if the type is an R type or native type.
 *
 * `a.flags_.includes(b.flags_)` is a necessary condition for `a :> b`.
 *
 * The "BaseType" is the (union) R or native type, stripped of all flags.
 *
 */

struct PirType {
    typedef EnumSet<RType> RTypeSet;
    typedef EnumSet<NativeType> NativeTypeSet;
    typedef EnumSet<TypeFlags> FlagSet;

    FlagSet flags_;

    union Type {
        RTypeSet r;
        NativeTypeSet n;
        constexpr Type(RTypeSet r) : r(r) {}
        constexpr Type(NativeTypeSet n) : n(n) {}
    };
    Type t_;

    static constexpr FlagSet defaultRTypeFlags() {
        return FlagSet() | TypeFlags::maybeNotScalar | TypeFlags::rtype;
    }

    static constexpr FlagSet topRTypeFlags() {
        return FlagSet() | TypeFlags::lazy | TypeFlags::promiseWrapped |
               TypeFlags::maybeObject | TypeFlags::maybeNotScalar |
               TypeFlags::rtype;
    }
    static constexpr FlagSet optimisticRTypeFlags() {
        return FlagSet() | TypeFlags::rtype;
    }

    static constexpr PirType optimistic() {
        PirType t;
        t.flags_ = optimisticRTypeFlags();
        return t;
    }

    constexpr PirType() : flags_(topRTypeFlags()), t_(RTypeSet()) {}
    // cppcheck-suppress noExplicitConstructor
    constexpr PirType(const RType& t) : flags_(defaultRTypeFlags()), t_(t) {}
    // cppcheck-suppress noExplicitConstructor
    constexpr PirType(const NativeType& t) : t_(t) {}
    // cppcheck-suppress noExplicitConstructor
    constexpr PirType(const RTypeSet& t) : flags_(defaultRTypeFlags()), t_(t) {}
    constexpr PirType(const RTypeSet& t, const FlagSet& f) : flags_(f), t_(t) {}
    explicit constexpr PirType(const NativeTypeSet& t) : t_(t) {}
    explicit PirType(SEXP);
    constexpr PirType(const PirType& other)
        : flags_(other.flags_), t_(other.t_) {}

    constexpr PirType& operator=(const PirType& o) {
        flags_ = o.flags_;
        if (isRType())
            t_.r = o.t_.r;
        else
            t_.n = o.t_.n;
        return *this;
    }

    RIR_INLINE bool merge(const PirType& other) {
        PirType t = *this;
        *this = *this | other;
        return *this != t;
    }

    void merge(const ObservedValues& other);
    void merge(SEXPTYPE t);

    static constexpr PirType num() {
        return PirType(RType::logical) | RType::integer | RType::real |
               RType::cplx;
    }
    static constexpr PirType val() {
        return PirType(vecs() | list() | RType::sym | RType::chr | RType::raw |
                       RType::closure | RType::prom | RType::code | RType::env |
                       RType::missing | RType::unbound | RType::ast)
            .orObject();
    }
    static constexpr PirType vecs() { return num() | RType::str | RType::vec; }
    static constexpr PirType closure() { return RType::closure; }

    static constexpr PirType promiseWrappedVal() {
        return val().orPromiseWrapped();
    }
    static constexpr PirType valOrLazy() { return val().orLazy(); }
    static constexpr PirType list() {
        return PirType(RType::cons) | RType::nil;
    }
    static constexpr PirType any() { return val().orLazy(); }

    RIR_INLINE bool maybeMissing() const {
        if (!isRType())
            return false;
        return t_.r.includes(RType::missing);
    }
    RIR_INLINE bool maybeLazy() const {
        if (!isRType())
            return false;
        return flags_.includes(TypeFlags::lazy);
    }
    RIR_INLINE bool maybePromiseWrapped() const {
        if (!isRType())
            return false;
        return flags_.includes(TypeFlags::promiseWrapped);
    }
    RIR_INLINE constexpr bool isScalar() const {
        if (!isRType())
            return true;
        return !flags_.includes(TypeFlags::maybeNotScalar);
    }
    RIR_INLINE constexpr bool isRType() const {
        return flags_.includes(TypeFlags::rtype);
    }
    RIR_INLINE constexpr bool maybe(RType type) const {
        return isRType() && t_.r.includes(type);
    }
    RIR_INLINE constexpr bool maybeObj() const {
        if (!isRType())
            return false;
        return isRType() && flags_.includes(TypeFlags::maybeObject);
    }

    RIR_INLINE constexpr PirType operator|(const PirType& o) const {
        assert(isRType() == o.isRType());

        PirType r;
        if (isRType())
            r = t_.r | o.t_.r;
        else
            r = RTypeSet(t_.n | o.t_.n);

        r.flags_ = flags_ | o.flags_;
        return r;
    }

    RIR_INLINE constexpr PirType operator&(const PirType& o) const {
        assert(isRType() == o.isRType());

        PirType r;
        if (isRType())
            r = t_.r & o.t_.r;
        else
            r = RTypeSet(t_.n & o.t_.n);

        r.flags_ = flags_ & o.flags_;
        return r;
    }

    bool maybeReferenceCounted() const {
        static constexpr RTypeSet refcount =
            RTypeSet() | RType::logical | RType::integer | RType::real |
            RType::str | RType::vec | RType::cplx;
        return isRType() && t_.r.intersects(refcount);
    }

    PirType notObject() const {
        assert(isRType());
        PirType t = *this;
        t.flags_.reset(TypeFlags::maybeObject);
        return t;
    }

    PirType notMissing() const {
        assert(isRType());
        PirType t = *this;
        t.t_.r.reset(RType::missing);
        return t;
    }

    RIR_INLINE PirType scalar() const {
        assert(isRType());
        PirType t = *this;
        t.flags_.reset(TypeFlags::maybeNotScalar);
        return t;
    }

    RIR_INLINE constexpr PirType orPromiseWrapped() const {
        assert(isRType());
        return PirType(t_.r, flags_ | TypeFlags::promiseWrapped);
    }

    RIR_INLINE constexpr PirType orLazy() const {
        assert(isRType());
        return PirType(t_.r,
                       flags_ | TypeFlags::lazy | TypeFlags::promiseWrapped);
    }

    RIR_INLINE constexpr PirType orObject() const {
        assert(isRType());
        return PirType(t_.r, flags_ | TypeFlags::maybeObject);
    }

    PirType constexpr forced() const {
        assert(isRType());
        FlagSet notPromised =
            ~(FlagSet() | TypeFlags::promiseWrapped | TypeFlags::lazy);
        return PirType(t_.r, flags_ & notPromised);
    }

    RIR_INLINE constexpr PirType baseType() const {
        assert(isRType());
        return PirType(t_.r);
    }

    RIR_INLINE void setNotMissing() { *this = notMissing(); }
    RIR_INLINE void setNotObject() { *this = notObject(); }
    RIR_INLINE void setScalar() { *this = scalar(); }

    static const PirType voyd() { return PirType(NativeTypeSet()); }
    static const PirType bottom() { return PirType(RTypeSet()); }

    RIR_INLINE bool operator==(const NativeType& o) const {
        return !isRType() && t_.n == o;
    }

    RIR_INLINE bool operator!=(const PirType& o) const { return !(*this == o); }

    RIR_INLINE bool operator==(const PirType& o) const {
        return flags_ == o.flags_ &&
               (isRType() ? t_.r == o.t_.r : t_.n == o.t_.n);
    }

    bool isA(const PirType& o) const { return o.isSuper(*this); }

    bool isSuper(const PirType& o) const {
        if (isRType() != o.isRType()) {
            return false;
        }
        if (!isRType()) {
            return t_.n.includes(o.t_.n);
        }
        if ((!maybeLazy() && o.maybeLazy()) ||
            (!maybePromiseWrapped() && o.maybePromiseWrapped()) ||
            (isScalar() && !o.isScalar())) {
            return false;
        }
        return t_.r.includes(o.t_.r);
    }

    void print(std::ostream& out = std::cout);
};

inline std::ostream& operator<<(std::ostream& out, NativeType t) {
    switch (t) {
    case NativeType::context:
        out << "ct";
        break;
    case NativeType::test:
        out << "t";
        break;
    case NativeType::checkpoint:
        out << "cp";
        break;
    case NativeType::frameState:
        out << "fs";
        break;
    case NativeType::_UNUSED_:
        assert(false);
        break;
    }
    return out;
}

inline std::ostream& operator<<(std::ostream& out, RType t) {
    switch (t) {
    case RType::ast:
        out << "ast";
        break;
    case RType::raw:
        out << "raw";
        break;
    case RType::vec:
        out << "vec";
        break;
    case RType::chr:
        out << "char";
        break;
    case RType::real:
        out << "real";
        break;
    case RType::cplx:
        out << "complex";
        break;
    case RType::str:
        out << "str";
        break;
    case RType::env:
        out << "env";
        break;
    case RType::code:
        out << "code";
        break;
    case RType::cons:
        out << "cons";
        break;
    case RType::prom:
        out << "prom";
        break;
    case RType::nil:
        out << "nil";
        break;
    case RType::closure:
        out << "cls";
        break;
    case RType::sym:
        out << "sym";
        break;
    case RType::integer:
        out << "int";
        break;
    case RType::logical:
        out << "lgl";
        break;
    case RType::missing:
        out << "miss";
        break;
    case RType::unbound:
        out << "_";
        break;
    case RType::_UNUSED_:
        assert(false);
        break;
    }
    return out;
}

inline std::ostream& operator<<(std::ostream& out, PirType t) {
    if (!t.isRType()) {
        if (t.t_.n.empty()) {
            out << "void";
            return out;
        }

        if (t.t_.n.count() > 1)
            out << "(";
        for (auto e = t.t_.n.begin(); e != t.t_.n.end(); ++e) {
            out << *e;
            if (e + 1 != t.t_.n.end())
                out << "|";
        }
        if (t.t_.n.count() > 1)
            out << ")";
        return out;
    }

    // If the base type is at least a value, then it's a value
    if (t.isRType() && PirType::val().notMissing().baseType() == t.baseType()) {
        out << "val";
    } else if (t.isRType() && PirType::val().baseType() == t.baseType()) {
        out << "val?";
    } else {
        if (t.t_.r.count() > 1)
            out << "(";
        for (auto i = t.t_.r.begin(); i != t.t_.r.end(); ++i) {
            out << *i;
            if (i + 1 != t.t_.r.end())
                out << "|";
        }
        if (t.t_.r.count() > 1)
            out << ")";
    }

    if (t.isScalar())
        out << "$";
    if (t.maybeLazy())
        out << "^";
    else if (t.maybePromiseWrapped())
        out << "~";
    if (!t.maybeObj())
        out << "'";

    return out;
}
} // namespace pir
} // namespace rir

#endif
