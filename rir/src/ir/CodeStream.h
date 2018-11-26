#ifndef RIR_CODE_STREAM_H
#define RIR_CODE_STREAM_H

#include <cstring>
#include <map>
#include <vector>

#include "runtime/Code.h"
#include "BC.h"

#include "CodeVerifier.h"
#include "utils/FunctionWriter.h"

namespace rir {

class CodeStream {

    friend class Compiler;

    std::vector<char>* code;

    typedef unsigned PcOffset;
    PcOffset pos = 0;
    unsigned size = 1024;
    unsigned nops = 0;

    FunctionWriter& function;

    SEXP ast;

    unsigned nextLabel = 0;

    // Ordered map on purpose, because FunctionWriter consumes them in order.
    // Patchpoints are positions in the BC stream that need to be patched. On
    // finalization those locations will be replaced by the physical byte offset
    // to the corresponding label.
    std::map<PcOffset, BC::Label> patchpoints;
    // Labels can be put at the start of any instruction. One instruction
    // can have multiple labels. Labels are unique, consecutive numbers
    // generated by mkLabel();
    std::map<PcOffset, std::vector<BC::Label>> labels;
    // Sources can be attached to any instruction. The current API is to call
    // addSrc, after emiting the instruction, so the PcOffsets are after the
    // instruction. The FunctionWriter will rewrite this and attach sources to
    // the beginning of an instruction in the final Code object.
    std::map<PcOffset, BC::PoolIdx> sources;

    uint32_t nextCallSiteIdx_ = 0;

  public:
    CodeStream(const CodeStream& other) = delete;
    CodeStream& operator=(const CodeStream& other) = delete;

    BC::Label mkLabel() {
        assert(nextLabel < BC::MAX_JMP);
        return nextLabel++;
    }

    void patchpoint(BC::Label l) {
        assert(patchpoints.count(pos) == 0 &&
               "Cannot have two patchpoints at the same position");
        patchpoints[pos] = l;
        insert((BC::Jmp)-1);
    }

    CodeStream(FunctionWriter& function, SEXP ast)
        : function(function), ast(ast) {
        code = new std::vector<char>(1024);
    }

    uint32_t alignedSize(uint32_t needed) {
        static const uint32_t align = 32;
        return (needed % align == 0) ? needed
                                     : needed + align - (needed % align);
    }

    CodeStream& operator<<(const BC& b) {
        if (b.bc == Opcode::nop_)
            nops++;
        b.write(*this);
        return *this;
    }

    CodeStream& operator<<(BC::Label label) {

        // get rid of unnecessary jumps
        {
            unsigned imm = pos - sizeof(BC::Jmp);
            unsigned op = imm - sizeof(Opcode);
            if (patchpoints.count(imm) && patchpoints[imm] == label) {
                switch ((Opcode)(*code)[op]) {
                case Opcode::br_:
                    remove(op);
                    break;
                default:
                    break;
                }
            }
        }

        labels[pos].push_back(label);
        return *this;
    }

    template <typename T>
    void insert(T val) {
        size_t s = sizeof(T);
        if (pos + s >= size) {
            size += 1024;
            code->resize(size);
        }
        memcpy(&(*code)[pos], &val, s);
        pos += s;
    }

    void addSrc(SEXP src) { sources[pos] = src_pool_add(globalContext(), src); }

    void addSrcIdx(unsigned idx) { sources[pos] = idx; }

    unsigned currentPos() const {
        return pos;
    }

    unsigned currentSourcesSize() const {
        return sources.size();
    }

    void remove(unsigned pc) {

#define INS(pc_) (reinterpret_cast<Opcode*>(&(*code)[(pc_)]))

        unsigned bcSize = BC::decodeShallow(INS(pc)).size();

        for (unsigned i = 0; i < bcSize; ++i) {
            *INS(pc + i) = Opcode::nop_;
            nops++;
            // patchpoints are fixed by just removing the binding to label
            patchpoints.erase(pc + i);
        }

        sources.erase(pc + bcSize);
    }

    BC::FunIdx finalize(bool markDefaultArg, size_t localsCnt) {
        Code* res =
            function.writeCode(ast, &(*code)[0], pos, sources, patchpoints,
                               labels, markDefaultArg, localsCnt, nops);

        labels.clear();
        patchpoints.clear();
        sources.clear();
        nextLabel = 0;
        nextCallSiteIdx_ = 0;

        delete code;
        code = nullptr;
        pos = 0;

        CodeVerifier::calculateAndVerifyStack(res);
        return res->index;
    }
};

} // namespace rir

#endif
