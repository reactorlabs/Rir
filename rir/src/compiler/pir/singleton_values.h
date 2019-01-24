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
  protected:
    SingletonValue(PirType t, Tag tag) : Value(t, tag) {}

  public:
    SingletonValue(const SingletonValue&) = delete;
    SingletonValue& operator=(const SingletonValue&) = delete;

    static T* instance() {
        static T i;
        return &i;
    }
};

class Nil : public SingletonValue<Nil> {
  public:
    void printRef(std::ostream& out) const override final { out << "nil"; }

  private:
    friend class SingletonValue;
    Nil() : SingletonValue(RType::nil, Tag::Nil) {}
};

class Missing : public SingletonValue<Missing> {
  public:
    void printRef(std::ostream& out) const override final { out << "missing"; }

  private:
    friend class SingletonValue;
    Missing() : SingletonValue(PirType::missing(), Tag::Missing) {}
};

class True : public SingletonValue<True> {
  public:
    void printRef(std::ostream& out) const override final { out << "true"; }

  private:
    friend class SingletonValue;
    True() : SingletonValue(NativeType::test, Tag::True) {}
};

class False : public SingletonValue<False> {
  public:
    void printRef(std::ostream& out) const override final { out << "false"; }

  private:
    friend class SingletonValue;
    False() : SingletonValue(NativeType::test, Tag::False) {}
};

class Tombstone : public Value {
  public:
    void printRef(std::ostream& out) const override final {
        out << "~";
        if (this == closure())
            out << "cls";
        else if (this == framestate())
            out << "fs";
        else
            assert(false);
    }
    static Tombstone* closure() {
        static Tombstone cls(RType::closure);
        return &cls;
    }
    static Tombstone* framestate() {
        static Tombstone fs(NativeType::frameState);
        return &fs;
    }

  private:
    explicit Tombstone(PirType t) : Value(t, Tag::Tombstone) {}
};
}
}

#endif
