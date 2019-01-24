#ifndef COMPILER_CLOSURE_VERSION_H
#define COMPILER_CLOSURE_VERSION_H

#include "../../runtime/Function.h"
#include "code.h"
#include "optimization_context.h"
#include "pir.h"
#include <functional>
#include <sstream>
#include <unordered_map>

namespace rir {
namespace pir {

/*
 * ClosureVersion
 *
 */
class ClosureVersion : public Code {
  public:
    enum class Property {
        IsEager,
        NoReflection,

        FIRST = IsEager,
        LAST = NoReflection
    };

    struct Properties : public EnumSet<Property> {
        Properties() : EnumSet<Property>(){};
        Properties(const EnumSet<Property>& other) : EnumSet<Property>(other) {}
        Properties(const Property& other) : EnumSet<Property>(other) {}
    };

  private:
    Closure* owner_;
    std::vector<Promise*> promises_;
    const OptimizationContext& optimizationContext_;

    std::string name_;
    std::string nameSuffix_;
    ClosureVersion(Closure* closure,
                   const OptimizationContext& optimizationContext,
                   const Properties& properties = Properties());

    friend class Closure;

  public:
    ClosureVersion* clone(const Assumptions& newAssumptions);

    const Assumptions& assumptions() const {
        return optimizationContext_.assumptions;
    }
    const OptimizationContext& optimizationContext() const {
        return optimizationContext_;
    }

    Properties properties;

    Closure* owner() const { return owner_; }
    size_t nargs() const;
    const std::string& name() const { return name_; }
    const std::string& nameSuffix() const { return nameSuffix_; }

    void print(std::ostream& out, bool tty) const;

    Promise* createProm(unsigned srcPoolIdx);

    Promise* promise(unsigned id) const { return promises_.at(id); }
    const std::vector<Promise*>& promises() { return promises_; }

    void erasePromise(unsigned id);

    typedef std::function<void(Promise*)> PromiseIterator;
    void eachPromise(PromiseIterator it) const {
        for (auto p : promises_)
            if (p)
                it(p);
    }

    size_t size() const override final;

    friend std::ostream& operator<<(std::ostream& out,
                                    const ClosureVersion& e) {
        out << e.name();
        return out;
    }

    ~ClosureVersion();
};

} // namespace pir
} // namespace rir

#endif
