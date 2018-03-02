#include "pir_impl.h"

#include <iostream>

namespace rir {
namespace pir {

BB::BB(Code* fun, unsigned id) : id(id), fun(fun) {}

void BB::print(std::ostream& out) {
    out << "BB " << id << "\n";
    for (auto i : instr) {
        out << "  ";
        i->print(out);
        out << "\n";
    }
    if (next0 && !next1) {
        out << "  goto BB" << next0->id << "\n";
    }
}

BB::~BB() {
    gc();
    for (auto* i : instr)
        delete i;
}

void BB::append(Instruction* i) {
    instr.push_back(i);
    i->bb_ = this;
}

BB::Instrs::iterator BB::remove(Instrs::iterator it) {
    deleted.push_back(*it);
    return instr.erase(it);
}

BB::Instrs::iterator BB::moveToEnd(Instrs::iterator it, BB* other) {
    Instruction* i = *it;
    i->bb_ = other;
    other->instr.push_back(i);
    return instr.erase(it);
}

BB::Instrs::iterator BB::moveToBegin(Instrs::iterator it, BB* other) {
    Instruction* i = *it;
    i->bb_ = other;
    other->instr.insert(other->instr.begin(), i);
    return instr.erase(it);
}

void BB::swap(Instrs::iterator it) {
    Instruction* i = *it;
    *it = *(it + 1);
    *(it + 1) = i;
}

BB* BB::cloneInstrs(BB* src, unsigned id, Code* target) {
    BB* c = new BB(target, id);
    for (auto i : src->instr) {
        Instruction* ic = i->clone();
        ic->bb_ = c;
        c->instr.push_back(ic);
    }
    c->next0 = c->next1 = nullptr;
    return c;
}

void BB::replace(Instrs::iterator it, Instruction* i) {
    deleted.push_back(*it);
    *it = i;
    i->bb_ = this;
}

BB::Instrs::iterator BB::insert(Instrs::iterator it, Instruction* i) {
    auto itup = instr.insert(it, i);
    i->bb_ = this;
    return itup;
}

void BB::gc() {
    // Catch double deletes
    std::set<Instruction*> dup;
    dup.insert(deleted.begin(), deleted.end());
    assert(dup.size() == deleted.size());

    for (auto i : deleted)
        delete i;
    deleted.clear();
}
}
}
