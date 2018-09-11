#include "Code.h"
#include "Function.h"
#include "R/Printing.h"
#include "ir/BC.h"
#include "utils/Pool.h"

#include <iomanip>

namespace rir {
Code::Code(FunctionSEXP fun, size_t index, SEXP ast, unsigned cs,
           unsigned sourceLength, bool isDefaultArg, size_t localsCnt)
    : function_(fun), index(index), src(src_pool_add(globalContext(), ast)),
      localsCount(localsCnt), codeSize(cs), srcLength(sourceLength),
      perfCounter(0), isDefaultArgument(isDefaultArg) {
    info.gc_area_start = sizeof(rir_header);
    info.gc_area_length = 1;
    info.magic = CODE_MAGIC;
}

Function* Code::function() { return Function::unpack(function_); }

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

    while (pc < endCode()) {
        BC bc = BC::decode(pc);

        const size_t OFFSET_WIDTH = 5;
        out << std::setw(OFFSET_WIDTH) << ((uintptr_t)pc - (uintptr_t)code());

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

        bc.print(out);

        pc = BC::next(pc);
    }
}

void Code::print(std::ostream& out) const {
    out << "Code object (" << this << " index " << index << ")\n";
    out << "   Source: " << src << " index to src pool\n";
    out << "   Magic: " << std::hex << info.magic << std::dec << "(hex)\n";
    out << "   Stack (o): " << stackLength << "\n";
    out << "   Code size: " << codeSize << "[B]\n";

    out << "   Default arg? " << (isDefaultArgument ? "yes\n" : "no") << "\n";
    if (info.magic != CODE_MAGIC) {
        out << "Wrong magic number -- corrupted IR bytecode";
        Rf_error("Wrong magic number -- corrupted IR bytecode");
    }

    out << "\n";
    disassemble(out);
}

FunctionCodeIterator::FunctionCodeIterator(Function const* const function,
                                           size_t index)
    : function(function), index(index) {}

void FunctionCodeIterator::operator++() { ++index; }

bool FunctionCodeIterator::operator!=(FunctionCodeIterator other) {
    return function != other.function || index != other.index;
}

Code* FunctionCodeIterator::operator*() {
    assert(0 <= index && index < function->codeLength &&
           "Iterator index out of bounds.");
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
    assert(0 <= index && index < function->codeLength &&
           "Iterator index out of bounds.");
    return Code::unpack(function->codeObjects[index]);
}
} // namespace rir
