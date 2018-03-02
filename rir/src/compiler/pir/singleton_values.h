#ifndef COMPILER_SINGLETON_VALUES_H
#define COMPILER_SINGLETON_VALUES_H

#include "instruction_list.h"
#include "tag.h"
#include "value.h"

#include <functional>
#include <iostream>

namespace rir {
namespace pir {

template <typename T>
class SingletonValue : public Value {
  public:
    SingletonValue(PirType t, Tag tag) : Value(t, tag) {}
    static T* instance() {
        static T i;
        return &i;
    }
};

class Nil : public SingletonValue<Nil> {
  public:
    void printRef(std::ostream& out) { out << "nil"; }

  private:
    friend class SingletonValue;
    Nil() : SingletonValue(RType::nil, Tag::Nil) {}
};

class Missing : public SingletonValue<Missing> {
  public:
    void printRef(std::ostream& out) { out << "missing"; }

  private:
    friend class SingletonValue;
    Missing() : SingletonValue(PirType::missing(), Tag::Missing) {}
};
}
}

#endif
