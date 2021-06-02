#ifndef COMPILER_PROMISE_H
#define COMPILER_PROMISE_H

#include "code.h"
#include "runtime/Code.h"

namespace rir {
namespace pir {

class LdFunctionEnv;

class Promise : public CodeImpl<CodeTag::Promise, Promise> {
  public:
    const unsigned id;
    ClosureVersion* owner;

    friend std::ostream& operator<<(std::ostream& out, const Promise& p) {
        out << "Prom(" << p.id << ")";
        return out;
    }

    rir::Code* rirSrc() const override final { return rirSrc_; }
    unsigned astIdx() const override final {
        return hasRirSrc() ? rirSrc()->src : ast_;
    }

    LdFunctionEnv* env() const;

    bool trivial() const;

  private:
    rir::Code* rirSrc_;
    unsigned ast_;
    friend class ClosureVersion;
    Promise(ClosureVersion* owner, unsigned id, rir::Code* rirSrc);
    Promise(ClosureVersion* owner, unsigned id, unsigned ast);
};

} // namespace pir
} // namespace rir

#endif
