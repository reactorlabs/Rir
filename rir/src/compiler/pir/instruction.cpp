#include "instruction.h"
#include "pir_impl.h"

#include "../util/visitor.h"
#include "R/Funtab.h"
#include "utils/capture_out.h"

#include <cassert>
#include <iomanip>
#include <sstream>

extern "C" SEXP deparse1line(SEXP call, Rboolean abbrev);

namespace rir {
namespace pir {

static size_t getMaxInstructionNameLength() {
    size_t max = 0;
    size_t cur;
#define V(n)                                                                   \
    cur = std::string(#n).length();                                            \
    if (cur > max)                                                             \
        max = cur;
    COMPILER_INSTRUCTIONS(V)
    return max;
}
static size_t maxInstructionNameLength = getMaxInstructionNameLength();

extern std::ostream& operator<<(std::ostream& out, Instruction::Id id) {
    out << std::get<0>(id) << "." << std::get<1>(id);
    return out;
}

void printPaddedInstructionName(std::ostream& out, const std::string& name) {
    out << std::left << std::setw(maxInstructionNameLength + 1) << name << " ";
}

void Instruction::print(std::ostream& out) {
    std::ostringstream buf;
    buf << type;
    out << std::left << std::setw(7) << buf.str() << " ";
    buf.str("");
    if (type != PirType::voyd()) {
        printRef(buf);
        out << std::setw(5) << buf.str() << " = ";
        buf.str("");
    } else {
        out << "        ";
    }

    printPaddedInstructionName(out, name());
    printArgs(buf);
    out << std::setw(50) << buf.str();

    if (leaksEnv())
        out << " ; env leak";
    else if (needsLiveEnv())
        out << " ; env live";
}

void Instruction::printRef(std::ostream& out) {
    if (type == RType::env)
        out << "e" << id();
    else
        out << "%" << id();
};

Instruction::Id Instruction::id() { return Id(bb()->id, bb()->indexOf(this)); }

bool Instruction::unused() {
    // TODO: better solution?
    if (tag == Tag::Branch || tag == Tag::Return)
        return false;

    return Visitor::check(bb(), [&](BB* bb) {
        bool unused = true;
        for (auto i : *bb) {
            i->each_arg(
                [&](Value* v, PirType t) { unused = unused && (v != this); });
            if (!unused)
                return false;
        }
        return unused;
    });
}

Instruction* Instruction::hasSingleUse() {
    size_t seen = 0;
    Instruction* usage;
    Visitor::check(bb(), [&](BB* bb) {
        for (auto i : *bb) {
            i->each_arg([&](Value* v, PirType t) {
                if (v == this) {
                    usage = i;
                    seen++;
                }
            });
            if (seen > 1)
                return false;
        }
        return true;
    });
    if (seen == 1)
        return usage;
    return nullptr;
}

void Instruction::replaceUsesIn(Value* replace, BB* target) {
    Visitor::run(target, [&](BB* bb) {
        for (auto i : *bb) {
            i->map_arg([&](Value** v) {
                if (*v == this)
                    *v = replace;
            });
        }
    });
}

void Instruction::replaceUsesWith(Value* replace) {
    Visitor::run(bb(), [&](BB* bb) {
        for (auto i : *bb) {
            i->map_arg([&](Value** v) {
                if (*v == this)
                    *v = replace;
            });
        }
    });
}

void LdConst::printArgs(std::ostream& out) {
    std::string val;
    {
        CaptureOut rec;
        Rf_PrintValue(c);
        val = rec();
    }
    if (val.length() > 0)
        val.pop_back();
    out << val;
}

void Branch::printArgs(std::ostream& out) {
    FixedLenInstruction::printArgs(out);
    out << " -> BB" << bb()->next0->id << " | BB" << bb()->next1->id;
}

void MkArg::printArgs(std::ostream& out) {
    out << "(";
    arg<0>()->printRef(out);
    out << ", " << *prom << ", ";
    env()->printRef(out);
    out << ") ";
}

void LdVar::printArgs(std::ostream& out) {
    out << "(" << CHAR(PRINTNAME(varName)) << ", ";
    env()->printRef(out);
    out << ") ";
}

void LdFun::printArgs(std::ostream& out) {
    out << "(" << CHAR(PRINTNAME(varName)) << ", ";
    env()->printRef(out);
    out << ") ";
}

void LdArg::printArgs(std::ostream& out) { out << "(" << id << ")"; }

void StVar::printArgs(std::ostream& out) {
    out << "(" << CHAR(PRINTNAME(varName)) << ", ";
    val()->printRef(out);
    out << ", ";
    env()->printRef(out);
    out << ") ";
}

void StVarSuper::printArgs(std::ostream& out) {
    out << "(" << CHAR(PRINTNAME(varName)) << ", ";
    val()->printRef(out);
    out << ", ";
    env()->printRef(out);
    out << ") ";
}

void LdVarSuper::printArgs(std::ostream& out) {
    out << "(" << CHAR(PRINTNAME(varName)) << ", ";
    env()->printRef(out);
    out << ") ";
}

void MkEnv::printArgs(std::ostream& out) {
    out << "(parent=";
    arg(0)->printRef(out);
    if (nargs() > 1)
        out << ", ";
    for (unsigned i = 0; i < nargs() - 1; ++i) {
        out << CHAR(PRINTNAME(this->varName[i])) << "=";
        this->arg(i + 1)->printRef(out);
        if (i != nargs() - 2)
            out << ", ";
    }
    out << ") ";
}

void Phi::updateType() {
    type = arg(0)->type;
    each_arg([&](Value* v, PirType t) -> void { type = type | v->type; });
}

void Phi::printArgs(std::ostream& out) {
    out << "(";
    if (nargs() > 0) {
        for (size_t i = 0; i < nargs(); ++i) {
            arg(i)->printRef(out);
            out << ":BB" << input[i]->id;
            if (i + 1 < nargs())
                out << ", ";
        }
    }
    out << ")";
}

CallSafeBuiltin::CallSafeBuiltin(SEXP builtin, const std::vector<Value*>& args)
    : VarLenInstruction(PirType::valOrLazy()), builtin(getBuiltin(builtin)),
      builtinId(getBuiltinNr(builtin)) {
    for (unsigned i = 0; i < args.size(); ++i)
        this->push_arg(PirType::val(), args[i]);
}

CallBuiltin::CallBuiltin(Value* e, SEXP builtin,
                         const std::vector<Value*>& args)
    : VarLenInstruction(PirType::valOrLazy(), e), builtin(getBuiltin(builtin)),
      builtinId(getBuiltinNr(builtin)) {
    for (unsigned i = 0; i < args.size(); ++i)
        this->push_arg(PirType::val(), args[i]);
}

void CallBuiltin::printArgs(std::ostream& out) {
    std::cout << "[" << getBuiltinName(builtinId) << "] ";
    Instruction::printArgs(out);
}

void CallSafeBuiltin::printArgs(std::ostream& out) {
    std::cout << "[" << getBuiltinName(builtinId) << "] ";
    Instruction::printArgs(out);
}

void Deopt::printArgs(std::ostream& out) {
    out << "@" << pc << ", stack=[";
    for (size_t i = 1; i < nargs(); ++i) {
        arg(i)->printRef(out);
        if (i + 1 < nargs())
            out << ", ";
    }
    out << "], env=";
    env()->printRef(out);
}
}
}
