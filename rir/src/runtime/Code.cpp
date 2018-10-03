#include "Code.h"
#include "Function.h"
#include "R/Printing.h"
#include "ir/BC.h"
#include "utils/Pool.h"

#include <iomanip>

namespace rir {
// cppcheck-suppress uninitMemberVar symbol=data
Code::Code(FunctionSEXP fun, size_t index, SEXP ast, unsigned cs,
           unsigned sourceLength, bool isDefaultArg, size_t localsCnt)
    : RirRuntimeObject(
          // GC area starts just after the header
          (intptr_t)&locals_ - (intptr_t)this,
          // GC area has only 1 pointer
          NumLocals),
      index(index), src(src_pool_add(globalContext(), ast)), stackLength(0),
      localsCount(localsCnt), codeSize(cs), srcLength(sourceLength),
      perfCounter(0), extraPoolSize(0), isDefaultArgument(isDefaultArg) {
    setEntry(0, fun);
    setEntry(1, R_NilValue);
}

Function* Code::function() { return Function::unpack(getEntry(0)); }

unsigned Code::getSrcIdxAt(const Opcode* pc, bool allowMissing) const {
    if (srcLength == 0) {
        assert(allowMissing);
        return 0;
    }

    SrclistEntry* sl = srclist();
    Opcode* start = code();
    auto pcOffset = pc - start;

    if (srcLength == 1) {
        auto sidx = sl[0].pcOffset == pcOffset ? sl[0].srcIdx : 0;
        SLOWASSERT(allowMissing || sidx);
        return sidx;
    }

    // Binary search through src list
    int lower = 0;
    int upper = srcLength - 1;
    int finger = upper / 2;
    unsigned sidx = 0;

    while (lower <= upper) {
        if (sl[finger].pcOffset == pcOffset) {
            sidx = sl[finger].srcIdx;
            break;
        }
        if (sl[finger].pcOffset < pcOffset)
            lower = finger + 1;
        else
            upper = finger - 1;
        finger = lower + (upper - lower) / 2;
    }
    SLOWASSERT(sidx == 0 || sl[finger].pcOffset == pcOffset);
    SLOWASSERT(allowMissing || sidx);

    return sidx;
}

void Code::disassemble(std::ostream& out) const {
    Opcode* pc = code();
    size_t label = 0;
    std::map<Opcode*, size_t> targets;
    targets[pc] = label++;
    while (pc < endCode()) {
        if (BC::decodeShallow(pc).isJmp()) {
            auto t = BC::jmpTarget(pc);
            if (!targets.count(t))
                targets[t] = label++;
        }
        pc = BC::next(pc);
    }
    // sort labels ascending
    label = 0;
    for (auto& t : targets)
        t.second = label++;

    auto formatLabel = [&](size_t label) { out << label; };

    pc = code();
    while (pc < endCode()) {

        if (targets.count(pc)) {
            formatLabel(targets[pc]);
            out << ":\n";
        }

        BC bc = BC::decode(pc, this);

        const size_t OFFSET_WIDTH = 7;
        out << std::right << std::setw(OFFSET_WIDTH)
            << ((uintptr_t)pc - (uintptr_t)code()) << std::left;

        unsigned s = getSrcIdxAt(pc, true);
        if (s != 0)
            out << "   ; " << dumpSexp(src_pool_at(globalContext(), s)) << "\n"
                << std::setw(OFFSET_WIDTH) << "";

        // Print call ast
        switch (bc.bc) {
        case Opcode::call_implicit_:
        case Opcode::named_call_implicit_:
        case Opcode::call_:
        case Opcode::named_call_:
            out << "   ; "
                << dumpSexp(Pool::get(bc.immediate.callFixedArgs.ast)).c_str()
                << "\n"
                << std::setw(OFFSET_WIDTH) << "";
            break;
        case Opcode::static_call_:
            out << "   ; "
                << dumpSexp(Pool::get(bc.immediate.staticCallFixedArgs.ast))
                       .c_str()
                << "\n"
                << std::setw(OFFSET_WIDTH) << "";
            break;
        default: {}
        }

        if (bc.isJmp()) {
            out << "   ";
            bc.printOpcode(out);
            formatLabel(targets[BC::jmpTarget(pc)]);
            out << "\n";
        } else {
            bc.print(out);
        }

        pc = BC::next(pc);
    }
}

void Code::print(std::ostream& out) const {
    out << "Code object (" << this << " index " << index << ")\n";
    out << std::left << std::setw(20) << "   Source: " << src
        << " (index into src pool)\n";
    out << std::left << std::setw(20) << "   Magic: " << std::hex << info.magic
        << std::dec << " (hex)\n";
    out << std::left << std::setw(20) << "   Stack (o): " << stackLength
        << "\n";
    out << std::left << std::setw(20) << "   Code size: " << codeSize
        << "[B]\n";
    out << std::left << std::setw(20) << "   Default arg? "
        << (isDefaultArgument ? "yes" : "no") << "\n";

    if (info.magic != CODE_MAGIC) {
        out << "Wrong magic number -- corrupted IR bytecode";
        Rf_error("Wrong magic number -- corrupted IR bytecode");
    }

    out << "\n";
    disassemble(out);
}

unsigned Code::addExtraPoolEntry(SEXP v) {
    SEXP cur = getEntry(1);
    unsigned curLen = cur == R_NilValue ? 0 : (unsigned)LENGTH(cur);
    if (curLen == extraPoolSize) {
        unsigned newCapacity = curLen ? curLen * 2 : 2;
        SEXP newPool = PROTECT(Rf_allocVector(VECSXP, newCapacity));
        for (unsigned i = 0; i < curLen; ++i) {
            SET_VECTOR_ELT(newPool, i, VECTOR_ELT(cur, i));
        }
        setEntry(1, newPool);
        UNPROTECT(1);
        cur = newPool;
    }
    SET_VECTOR_ELT(cur, extraPoolSize, v);
    return extraPoolSize++;
}

FunctionCodeIterator::FunctionCodeIterator(Function const* const function,
                                           size_t index)
    : function(function), index(index) {}

void FunctionCodeIterator::operator++() { ++index; }

bool FunctionCodeIterator::operator!=(FunctionCodeIterator other) {
    return function != other.function || index != other.index;
}

Code* FunctionCodeIterator::operator*() {
    assert(index < function->codeLength && "Iterator index out of bounds.");
    return Code::unpack(function->codeObjects[index]);
}

ConstFunctionCodeIterator::ConstFunctionCodeIterator(
    Function const* const function, size_t index)
    : function(function), index(index) {}

void ConstFunctionCodeIterator::operator++() { ++index; }

bool ConstFunctionCodeIterator::operator!=(ConstFunctionCodeIterator other) {
    return function != other.function || index != other.index;
}

const Code* ConstFunctionCodeIterator::operator*() {
    assert(index < function->codeLength && "Iterator index out of bounds.");
    return Code::unpack(function->codeObjects[index]);
}
} // namespace rir
