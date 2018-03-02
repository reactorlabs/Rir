#ifndef COMPILER_CODE_H
#define COMPILER_CODE_H

#include "pir.h"

namespace rir {
namespace pir {

/*
 * A piece of code, starting at the BB entry.
 *
 * Currently: either a Promise of a function.
 *
 */
class Code {
  public:
    BB* entry;

    Code();
    void print(std::ostream&);
    void print() { print(std::cerr); }
    ~Code();
};
}
}

#endif
