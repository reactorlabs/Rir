#ifndef RIR__PIR_COMPILER_H
#define RIR__PIR_COMPILER_H

#include "../pir/closure.h"
#include "../pir/module.h"
#include "../pir/value.h"
#include "pir_translator.h"
#include "runtime/Function.h"
#include <vector>

namespace rir {
namespace pir {

class RirCompiler {
  public:
    RirCompiler(Module* module) : module(module) {}
    RirCompiler(const RirCompiler&) = delete;
    void operator=(const RirCompiler&) = delete;

    virtual Closure* compileClosure(SEXP) = 0;

    bool isVerbose() { return verbose; }
    void setVerbose(bool v) { verbose = v; }

  private:
    bool verbose = false;

  protected:
    std::vector<PirTranslator*> translations;
    Module* module;
};
} // namespace pir
} // namespace rir

#endif
