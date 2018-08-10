#ifndef COMPILER_PROMISE_H
#define COMPILER_PROMISE_H

#include "code.h"

namespace rir {
namespace pir {

class Promise : public Code {
  public:
    unsigned id;
    Closure* fun;
    unsigned srcPoolIdx;

    void print(std::ostream& out = std::cout) {
        out << "Prom " << id << ":\n";
        Code::print(out);
    }

    friend std::ostream& operator<<(std::ostream& out, const Promise& p) {
        out << "Prom(" << p.id << ")";
        return out;
    }

  private:
    friend class Closure;
    Promise(Closure* fun, unsigned id, unsigned src)
        : id(id), fun(fun), srcPoolIdx(src) {}
};

} // namespace pir
} // namespace rir

#endif
