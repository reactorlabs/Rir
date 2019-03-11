#include "pir_2_rir.h"
#include "../../analysis/last_env.h"
#include "../../pir/pir_impl.h"
#include "../../transform/bb.h"
#include "../../util/cfg.h"
#include "../../util/visitor.h"
#include "interpreter/instance.h"
#include "ir/CodeStream.h"
#include "ir/CodeVerifier.h"
#include "runtime/DispatchTable.h"
#include "simple_instruction_list.h"
#include "stack_use.h"
#include "utils/FunctionWriter.h"

#include "../../debugging/PerfCounter.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace rir {
namespace pir {

namespace {

/*
 * SSAAllocator assigns each instruction to a local variable number, or the
 * stack. It uses the following algorithm:
 *
 * 1. Split phis with moves. This translates the IR to CSSA (see toCSSA).
 * 2. Compute liveness (see computeLiveness):
 *    Liveness intervals are stored as:
 *        Instruction* -> BB id -> { start : pos, end : pos, live : bool}
 *    Two Instructions interfere iff there is a BB where they are both live
 *    and the start-end overlap.
 * 3. For now, just put everything on stack. (step 4 is thus skipped...)
 * 4. Assign the remaining Instructions to local RIR variable numbers
 *    (see computeAllocation):
 *    1. Coalesc all remaining phi with their inputs. This is save since we are
 *       already in CSSA. Directly allocate a register on the fly, such that.
 *    2. Traverse the dominance tree and eagerly allocate the remaining ones
 * 5. For debugging, verify the assignment with a static analysis that simulates
 *    the variable and stack usage (see verify).
 */
class SSAAllocator {
  public:
    CFG cfg;
    DominanceGraph dom;
    Code* code;
    size_t bbsSize;

    LivenessIntervals livenessIntervals;
    StackUseAnalysis sa;

    typedef size_t SlotNumber;
    const static SlotNumber unassignedSlot = 0;
    const static SlotNumber stackSlot = -1;

    std::unordered_map<Value*, SlotNumber> allocation;

    explicit SSAAllocator(Code* code, ClosureVersion* cls, LogStream& log)
        : cfg(code), dom(code), code(code), bbsSize(code->nextBBId),
          livenessIntervals(bbsSize, cfg),
          sa(cls, code, log, livenessIntervals) {

        computeStackAllocation();
        computeAllocation();
    }

    void computeStackAllocation() {

        static auto toStack = [](Instruction* i) -> bool {
            return Phi::Cast(i) || !MkEnv::Cast(i);
        };

        std::unordered_set<Value*> phis;

        Visitor::run(code->entry, [&](Instruction* i) {
            auto p = Phi::Cast(i);
            if (!p || allocation.count(p))
                return;
            if (toStack(p)) {
                allocation[p] = stackSlot;
                p->eachArg(
                    [&](BB*, Value* v) { allocation[v] = allocation[p]; });
            } else {
                phis.insert(p);
                p->eachArg([&](BB*, Value* v) { phis.insert(v); });
            }
        });

        Visitor::run(code->entry, [&](Instruction* i) {
            if (allocation.count(i))
                return;
            if (toStack(i) && phis.count(i) == 0) {
                allocation[i] = stackSlot;
            }
        });
    }

    void computeAllocation() {
        std::unordered_map<SlotNumber, std::unordered_set<Value*>> reverseAlloc;
        auto slotIsAvailable = [&](SlotNumber slot, Value* i) {
            for (auto other : reverseAlloc[slot])
                if (livenessIntervals.at(other).interfere(
                        livenessIntervals.at(i)))
                    return false;
            return true;
        };

        // Precolor Phi
        Visitor::run(code->entry, [&](Instruction* i) {
            auto p = Phi::Cast(i);
            if (!p || allocation.count(p))
                return;
            SlotNumber slot = unassignedSlot;
            while (true) {
                ++slot;
                bool success = slotIsAvailable(slot, p);
                if (success) {
                    p->eachArg([&](BB*, Value* v) {
                        if (!slotIsAvailable(slot, v))
                            success = false;
                    });
                }
                if (success)
                    break;
            }
            allocation[i] = slot;
            reverseAlloc[slot].insert(i);
            p->eachArg([&](BB*, Value* v) {
                allocation[v] = slot;
                reverseAlloc[slot].insert(v);
            });
        });

        // Traverse the dominance graph in preorder and eagerly assign slots.
        // We assume that no critical paths exist, ie. we preprocessed the graph
        // such that every phi input is only used exactly once (by the phi).
        DominatorTreeVisitor<>(dom).run(code, [&](BB* bb) {
            auto findFreeSlot = [&](Instruction* i) {
                SlotNumber slot = unassignedSlot;
                for (;;) {
                    ++slot;
                    if (slotIsAvailable(slot, i)) {
                        allocation[i] = slot;
                        reverseAlloc[slot].insert(i);
                        break;
                    }
                };
            };

            size_t pos = 0;
            for (auto i : *bb) {
                ++pos;

                if (!allocation.count(i) && livenessIntervals.count(i)) {
                    // Try to reuse input slot, to reduce moving
                    SlotNumber hint = unassignedSlot;
                    if (i->nargs() > 0) {
                        auto o = Instruction::Cast(i->arg(0).val());
                        if (o && allocation.count(o))
                            hint = allocation.at(o);
                    }
                    if (hint != unassignedSlot && hint != stackSlot &&
                        slotIsAvailable(hint, i)) {
                        allocation[i] = hint;
                        reverseAlloc[hint].insert(i);
                    } else {
                        findFreeSlot(i);
                    }
                }
            }
        });
    }

    void print(std::ostream& out) {
        out << "Allocation\n";
        for (auto a : allocation) {
            out << "  ";
            a.first->printRef(out);
            out << ": ";
            if (onStack(a.first))
                out << "stack";
            else
                out << a.second;
            out << "\n";
        }
        out << "  dead: ";
        BreadthFirstVisitor::run(code->entry, [&](Instruction* i) {
            if (!hasSlot(i)) {
                i->printRef(out);
                out << " ";
            }
        });
        out << "\n"
            << "  # slots: " << slots() << "\n";
    }

    void verify() {

        // Explore all possible traces and verify the allocation
        typedef std::pair<BB*, BB*> Jmp;
        typedef std::unordered_map<size_t, Instruction*> RegisterFile;
        typedef std::deque<Instruction*> Stack;
        typedef std::function<void(BB*, BB*, RegisterFile&, Stack&)> VerifyBB;
        std::set<Jmp> branchTaken;

        VerifyBB verifyBB = [&](BB* pred, BB* bb, RegisterFile& reg,
                                Stack& stack) {
            for (auto i : *bb) {
                for (auto drop : sa.toDrop(i)) {
                    for (auto it = stack.begin(); it != stack.end(); ++it) {
                        if (drop == *it) {
                            stack.erase(it);
                            break;
                        }
                    }
                }
                if (auto phi = Phi::Cast(i)) {
                    SlotNumber slot = allocation.at(phi);
                    phi->eachArg([&](BB*, Value* a) {
                        auto ia = Instruction::Cast(a);
                        if (!ia)
                            return;
                        if (!allocation.count(ia)) {
                            std::cerr << "REG alloc fail: ";
                            phi->printRef(std::cerr);
                            std::cerr << " needs ";
                            ia->printRef(std::cerr);
                            std::cerr << " but is not allocated\n";
                            assert(false);
                        } else if (allocation[ia] != slot) {
                            std::cerr << "REG alloc fail: ";
                            phi->printRef(std::cerr);
                            std::cerr << " and it's input ";
                            ia->printRef(std::cerr);
                            std::cerr << " have different allocations: ";
                            if (allocation[phi] == stackSlot)
                                std::cerr << "stack";
                            else
                                std::cerr << allocation[phi];
                            std::cerr << " vs ";
                            if (allocation[ia] == stackSlot)
                                std::cerr << "stack";
                            else
                                std::cerr << allocation[ia];
                            std::cerr << "\n";
                            assert(false);
                        }
                    });
                    // Make sure the argument slot is initialized
                    if (slot != stackSlot && reg.count(slot) == 0) {
                        std::cerr << "REG alloc fail: phi ";
                        phi->printRef(std::cerr);
                        std::cerr << " is reading from an unititialized slot\n";
                        assert(false);
                    }
                    if (slot == stackSlot) {
                        bool found = false;
                        for (auto it = stack.begin(); it != stack.end(); ++it) {
                            phi->eachArg([&](BB* phiInput, Value* phiArg) {
                                if (phiInput == pred && phiArg == *it) {
                                    stack.erase(it);
                                    found = true;
                                }
                            });
                            if (found)
                                break;
                        }
                        if (!found) {
                            std::cerr << "REG alloc fail: phi ";
                            phi->printRef(std::cerr);
                            std::cerr << " input is missing on stack\n";
                            assert(false);
                        }
                    }
                } else {
                    // Make sure all our args are live
                    size_t argNum = 0;
                    i->eachArg([&](Value* a) {
                        auto ia = Instruction::Cast(a);
                        if (!ia || !ia->producesRirResult()) {
                            argNum++;
                            return;
                        }
                        if (!allocation.count(ia)) {
                            std::cerr << "REG alloc fail: ";
                            i->printRef(std::cerr);
                            std::cerr << " needs ";
                            ia->printRef(std::cerr);
                            std::cerr << " but is not allocated\n";
                            assert(false);
                        } else {
                            SlotNumber slot = allocation.at(ia);
                            if (slot == stackSlot) {
                                bool found = false;
                                for (auto it = stack.begin(); it != stack.end();
                                     ++it) {
                                    if (ia == *it) {
                                        found = true;
                                        if (lastUse(i, argNum)) {
                                            stack.erase(it);
                                        }
                                        break;
                                    }
                                }
                                if (!found) {
                                    std::cerr << "REG alloc fail: ";
                                    i->printRef(std::cerr);
                                    std::cerr << " needs ";
                                    ia->printRef(std::cerr);
                                    std::cerr << " but it's missing on stack\n";
                                    assert(false);
                                }
                            } else {
                                // Make sure the argument slot is initialized
                                if (reg.count(slot) == 0) {
                                    std::cerr << "REG alloc fail: ";
                                    i->printRef(std::cerr);
                                    std::cerr << " is reading its argument ";
                                    ia->printRef(std::cerr);
                                    std::cerr << "from an unititialized slot\n";
                                    assert(false);
                                }
                                if (reg.at(slot) != ia) {
                                    std::cerr << "REG alloc fail: ";
                                    i->printRef(std::cerr);
                                    std::cerr << " needs ";
                                    ia->printRef(std::cerr);
                                    std::cerr << " but slot " << slot
                                              << " was overridden by ";
                                    reg.at(slot)->printRef(std::cerr);
                                    std::cerr << "\n";
                                    assert(false);
                                }
                            }
                        }
                        argNum++;
                    });
                }

                // Remember this instruction if it writes to a slot
                if (allocation.count(i)) {
                    if (allocation.at(i) == stackSlot) {
                        if (i->producesRirResult() && !sa.dead(i)) {
                            stack.push_back(i);
                        }
                    } else {
                        reg[allocation.at(i)] = i;
                    }
                }
            }

            if (bb->isExit()) {
                if (stack.size() != 0) {
                    std::cerr << "REG alloc fail: BB" << bb->id
                              << " tries to return with " << stack.size()
                              << " elements on the stack\n";
                    assert(false);
                }
            }

            if (bb->trueBranch() &&
                !branchTaken.count(Jmp(bb, bb->trueBranch()))) {
                branchTaken.insert(Jmp(bb, bb->trueBranch()));
                if (!bb->falseBranch()) {
                    verifyBB(bb, bb->trueBranch(), reg, stack);
                } else {
                    // Need to copy here, since we are gonna explore
                    // falseBranch() next
                    RegisterFile regC = reg;
                    Stack stackC = stack;
                    verifyBB(bb, bb->trueBranch(), regC, stackC);
                }
            }
            if (bb->falseBranch() &&
                !branchTaken.count(Jmp(bb, bb->falseBranch()))) {
                branchTaken.insert(Jmp(bb, bb->falseBranch()));
                verifyBB(bb, bb->falseBranch(), reg, stack);
            }
        };

        {
            RegisterFile f;
            Stack s;
            verifyBB(nullptr, code->entry, f, s);
        }
    }

    size_t operator[](Value* v) const {
        assert(allocation.at(v) != stackSlot);
        return allocation.at(v) - 1;
    }

    size_t slots() const {
        unsigned max = 0;
        for (auto a : allocation) {
            if (a.second != stackSlot && max < a.second)
                max = a.second;
        }
        return max;
    }

    bool onStack(Value* v) const { return allocation.at(v) == stackSlot; }

    bool hasSlot(Value* v) const { return allocation.count(v); }

    size_t getStackOffset(Instruction* instr, std::vector<bool>& used,
                          Value* what, bool remove) const {

        auto stack = sa.stackBefore(instr);
        assert(stack.size() == used.size());

        size_t offset = 0;
        size_t usedIdx = used.size() - 1;
        auto i = stack.rbegin();
        while (i != stack.rend()) {
            if (*i == what) {
                if (remove) {
                    used[usedIdx] = true;
                }
                return offset;
            }
            if (hasSlot(*i) && onStack(*i) && !used[usedIdx]) {
                ++offset;
            }
            ++i;
            --usedIdx;
        }
        assert(false && "Value wasn't found on the stack.");
        return -1;
    }

    size_t stackPhiOffset(Instruction* instr, Phi* phi) const {
        auto stack = sa.stackBefore(instr);
        size_t offset = 0;
        for (auto i = stack.rbegin(); i != stack.rend(); ++i) {
            if (*i == phi || phi->anyArg([&](Value* v) { return *i == v; }))
                return offset;
            if (hasSlot(*i) && onStack(*i))
                ++offset;
        }
        assert(false && "Phi wasn't found on the stack.");
        return -1;
    }

    // Check if v is needed after argument argNumber of instr
    bool lastUse(Instruction* instr, size_t argNumber) const {
        assert(argNumber < instr->nargs());
        Value* v = instr->arg(argNumber).val();
        auto stack = sa.stackAfter(instr);
        for (auto i = stack.begin(); i != stack.end(); ++i)
            if (*i == v)
                return false;
        for (size_t i = argNumber + 1; i < instr->nargs(); ++i)
            if (instr->arg(i).val() == v)
                return false;
        return true;
    }
};

class Context {
  public:
    std::stack<CodeStream*> css;
    FunctionWriter& fun;

    explicit Context(FunctionWriter& fun) : fun(fun) {}
    ~Context() { assert(css.empty()); }

    CodeStream& cs() { return *css.top(); }

    rir::Code* finalizeCode(size_t localsCnt) {
        auto res = cs().finalize(localsCnt);
        delete css.top();
        css.pop();
        return res;
    }

    void push(SEXP ast) { css.push(new CodeStream(fun, ast)); }
};

class Pir2Rir {
  public:
    Pir2Rir(Pir2RirCompiler& cmp, ClosureVersion* cls, bool dryRun,
            LogStream& log)
        : compiler(cmp), cls(cls), dryRun(dryRun), log(log) {}
    size_t compileCode(Context& ctx, Code* code);
    rir::Code* getPromise(Context& ctx, Promise* code);

    void lower(Code* code);
    void toCSSA(Code* code);
    rir::Function* finalize();

  private:
    Pir2RirCompiler& compiler;
    ClosureVersion* cls;
    std::unordered_map<Promise*, rir::Code*> promises;
    bool dryRun;
    LogStream& log;
};

size_t Pir2Rir::compileCode(Context& ctx, Code* code) {
    lower(code);
    toCSSA(code);
    log.CSSA(code);

    SSAAllocator alloc(code, cls, log);
    log.afterAllocator(code, [&](std::ostream& o) { alloc.print(o); });
    alloc.verify();

    auto isJumpThrough = [&](BB* bb) {
        return bb->isEmpty() || (bb->size() == 1 && Nop::Cast(bb->last()) &&
                                 alloc.sa.toDrop(bb->last()).empty());
    };

    // Create labels for all bbs
    std::unordered_map<BB*, BC::Label> bbLabels;
    BreadthFirstVisitor::run(code->entry, [&](BB* bb) {
        if (!isJumpThrough(bb))
            bbLabels[bb] = ctx.cs().mkLabel();
    });

    LastEnv lastEnv(cls, code, log);
    std::unordered_map<Value*, BC::Label> pushContexts;

    LoweringVisitor::run(code->entry, [&](BB* bb) {
        if (isJumpThrough(bb))
            return;

        CodeStream& cs = ctx.cs();

        auto jumpThroughEmpty = [&](BB* bb) {
            while (isJumpThrough(bb))
                bb = bb->next();
            return bb;
        };

        cs << bbLabels[bb];

        for (auto it = bb->begin(); it != bb->end(); ++it) {
            auto instr = *it;

            // Prepare stack and araguments
            {

                std::vector<bool> removedStackValues(
                    alloc.sa.stackBefore(instr).size(), false);
                size_t pushedStackValues = 0;

                // Put args to buffer first so that we don't have to clean up
                // the code stream later
                std::vector<BC> buffer;
                std::vector<SEXP> srcBuffer;

#ifdef ENABLE_SLOWASSERT
                auto debugAddVariableName = [&](Value* v) {
                    std::stringstream ss;
                    v->printRef(ss);
                    srcBuffer.push_back(Rf_install(ss.str().c_str()));
                };
#else
        auto debugAddVariableName = [](Value*) {};
#endif

                /* TODO: there are still more patterns that could be cleaned up:
                 *   pick(2) swap() pick(2) -> swap()
                 *   pick(2) pop() swap() pop() pop() -> pop() pop() pop()
                 * also maybe:
                 * args being last use and matching the stack contents at tos
                 * then no picks are needed, only fill in the values / locals
                 * and put those to correct offsets
                 */

                auto explicitEnvValue = [](Instruction* instr) {
                    return MkEnv::Cast(instr) || IsEnvStub::Cast(instr);
                };

                auto moveToTOS = [&](size_t offset) {
                    if (offset == 1) {
                        // Get rid of double swaps
                        if (!buffer.empty() &&
                            buffer.back().is(rir::Opcode::swap_)) {
                            buffer.pop_back();
                        } else {
                            buffer.emplace_back(BC::swap());
                        }
                    } else if (offset > 1) {
                        // Get rid of n-fold pick(n)'s
                        if (buffer.size() >= offset) {
                            bool pickN = true;
                            for (auto it = buffer.rbegin(); it != buffer.rend();
                                 ++it)
                                if (!it->is(rir::Opcode::pick_) ||
                                    it->immediate.i != offset) {
                                    pickN = false;
                                    break;
                                }
                            if (pickN) {
                                while (offset--)
                                    buffer.pop_back();
                            } else {
                                buffer.emplace_back(BC::pick(offset));
                            }
                        } else {
                            buffer.emplace_back(BC::pick(offset));
                        }
                    }
                };

                auto copyToTOS = [&](size_t offset) {
                    if (offset == 0)
                        buffer.emplace_back(BC::dup());
                    else
                        buffer.emplace_back(BC::pull(offset));
                };

                auto getFromStack = [&](Value* what, size_t argNumber) {
                    auto lastUse = alloc.lastUse(instr, argNumber);
                    auto offset =
                        alloc.getStackOffset(instr, removedStackValues, what,
                                             lastUse) +
                        pushedStackValues;
                    if (lastUse)
                        moveToTOS(offset);
                    else
                        copyToTOS(offset);
                };

                auto loadEnv = [&](Value* what, size_t argNumber) {
                    if (what == Env::notClosed()) {
                        buffer.emplace_back(BC::parentEnv());
                    } else if (what == Env::nil()) {
                        buffer.emplace_back(BC::push(R_NilValue));
                    } else if (Env::isStaticEnv(what)) {
                        auto env = Env::Cast(what);
                        // Here we could also load env->rho, but if the user
                        // were to change the environment on the closure our
                        // code would be wrong.
                        if (env == cls->owner()->closureEnv())
                            buffer.emplace_back(BC::parentEnv());
                        else
                            buffer.emplace_back(BC::push(env->rho));
                    } else {
                        if (!alloc.hasSlot(what)) {
                            std::cerr << "Don't know how to load the env ";
                            what->printRef(std::cerr);
                            std::cerr << " (" << tagToStr(what->tag) << ")\n";
                            assert(false);
                        }
                        if (alloc.onStack(what)) {
                            getFromStack(what, argNumber);
                        } else {
                            buffer.emplace_back(BC::ldloc(alloc[what]));
                            debugAddVariableName(what);
                        }
                    }
                    pushedStackValues++;
                };

                auto loadArg = [&](Value* what, size_t argNumber) {
                    if (what == UnboundValue::instance()) {
                        assert(MkArg::Cast(instr) &&
                               "only mkarg supports R_UnboundValue");
                        buffer.emplace_back(BC::push(R_UnboundValue));
                    } else if (what == MissingArg::instance()) {
                        buffer.emplace_back(BC::push(R_MissingArg));
                    } else if (what == True::instance()) {
                        buffer.emplace_back(BC::push(R_TrueValue));
                    } else if (what == False::instance()) {
                        buffer.emplace_back(BC::push(R_FalseValue));
                    } else {
                        if (!alloc.hasSlot(what)) {
                            std::cerr << "Don't know how to load the arg ";
                            what->printRef(std::cerr);
                            std::cerr << " (" << tagToStr(what->tag) << ")\n";
                            assert(false);
                        }
                        if (alloc.onStack(what)) {
                            getFromStack(what, argNumber);
                        } else {
                            buffer.emplace_back(BC::ldloc(alloc[what]));
                            debugAddVariableName(what);
                        }
                    }
                    pushedStackValues++;
                };

                auto loadPhiArg = [&](Phi* phi) {
                    if (!alloc.hasSlot(phi)) {
                        std::cerr << "Don't know how to load the phi arg ";
                        phi->printRef(std::cerr);
                        std::cerr << " (" << tagToStr(phi->tag) << ")\n";
                        assert(false);
                    }
                    if (alloc.onStack(phi)) {
                        auto offset = alloc.stackPhiOffset(instr, phi);
                        moveToTOS(offset);
                    } else {
                        buffer.emplace_back(BC::ldloc(alloc[phi]));
                        debugAddVariableName(phi);
                    }
                };

                // Remove values from the stack that are dead here
                for (auto val : alloc.sa.toDrop(instr)) {
                    // If not actually allocated on stack, do nothing
                    if (!alloc.onStack(val))
                        continue;

                    auto offset = alloc.getStackOffset(
                        instr, removedStackValues, val, true);
                    moveToTOS(offset);
                    buffer.emplace_back(BC::pop());
                }

                if (auto phi = Phi::Cast(instr)) {
                    loadPhiArg(phi);
                } else {
                    size_t argNumber = 0;
                    instr->eachArg([&](Value* what) {
                        if (what == Env::elided() ||
                            what->tag == Tag::Tombstone ||
                            what->type == NativeType::context) {
                            argNumber++;
                            return;
                        }

                        if (instr->hasEnv() && instr->env() == what) {
                            if (explicitEnvValue(instr)) {
                                loadEnv(what, argNumber);
                            } else {
                                auto env = instr->env();
                                if (!lastEnv.envStillValid(instr)) {
                                    loadEnv(env, argNumber);
                                    buffer.emplace_back(BC::setEnv());
                                } else {
                                    if (alloc.hasSlot(env) &&
                                        alloc.onStack(env) &&
                                        alloc.lastUse(instr, argNumber)) {
                                        auto offset =
                                            alloc.getStackOffset(
                                                instr, removedStackValues, env,
                                                true) +
                                            pushedStackValues;
                                        moveToTOS(offset);
                                        buffer.emplace_back(BC::pop());
                                    }
                                }
                            }
                        } else {
                            loadArg(what, argNumber);
                        }
                        argNumber++;
                    });
                }

                unsigned srcBufferIdx = 0;
                for (auto const& bc : buffer) {
                    cs << bc;
                    if (srcBufferIdx < srcBuffer.size() &&
                        bc.is(rir::Opcode::ldloc_)) {
                        cs.addSrc(srcBuffer[srcBufferIdx]);
                        srcBufferIdx++;
                    }
                }
            }

            switch (instr->tag) {

            case Tag::LdConst: {
                cs << BC::push_from_pool(LdConst::Cast(instr)->idx);
                break;
            }

            case Tag::LdFun: {
                auto ldfun = LdFun::Cast(instr);
                cs << BC::ldfun(ldfun->varName);
                break;
            }

            case Tag::LdVar: {
                auto ldvar = LdVar::Cast(instr);
                cs << BC::ldvarNoForce(ldvar->varName);
                break;
            }

            case Tag::ForSeqSize: {
                cs << BC::forSeqSize();
                // TODO: currently we always pop the sequence, since we
                // cannot deal with instructions that do not pop the value
                // after use.
                cs << BC::swap() << BC::pop();
                break;
            }

            case Tag::LdArg: {
                auto ld = LdArg::Cast(instr);
                cs << BC::ldarg(ld->id);
                break;
            }

            case Tag::StVarSuper: {
                auto stvar = StVarSuper::Cast(instr);
                cs << BC::stvarSuper(stvar->varName);
                break;
            }

            case Tag::LdVarSuper: {
                auto ldvar = LdVarSuper::Cast(instr);
                cs << BC::ldvarNoForceSuper(ldvar->varName);
                break;
            }

            case Tag::StVar: {
                auto stvar = StVar::Cast(instr);
                if (stvar->isStArg)
                    cs << BC::starg(stvar->varName);
                else
                    cs << BC::stvar(stvar->varName);
                break;
            }

            case Tag::Branch: {
                auto trueBranch = jumpThroughEmpty(bb->trueBranch());
                auto falseBranch = jumpThroughEmpty(bb->falseBranch());
                // cs << BC::brtrue(bbLabels[trueBranch])
                //    << BC::br(bbLabels[falseBranch]);
                // this version looks better on a microbenchmark.. need to
                // investigate
                cs << BC::brfalse(bbLabels[falseBranch])
                   << BC::br(bbLabels[trueBranch]);

                // This is the end of this BB
                return;
            }

            case Tag::MkArg: {
                cs << BC::promise(
                    cs.addPromise(getPromise(ctx, MkArg::Cast(instr)->prom())));
                break;
            }

            case Tag::MkFunCls: {
                // TODO: would be nice to compile the function here. But I am
                // not sure if our compiler backend correctly deals with not
                // closed closures.
                auto mkfuncls = MkFunCls::Cast(instr);
                auto cls = mkfuncls->cls;
                cs << BC::push(cls->formals().original())
                   << BC::push(mkfuncls->originalBody->container())
                   << BC::push(cls->srcRef()) << BC::close();
                break;
            }

            case Tag::Missing: {
                auto m = Missing::Cast(instr);
                cs << BC::missing(m->varName);
                break;
            }

            case Tag::Is: {
                auto is = Is::Cast(instr);
                cs << BC::is(is->sexpTag);
                break;
            }

#define EMPTY(Name)                                                            \
    case Tag::Name: {                                                          \
        break;                                                                 \
    }
                EMPTY(CastType);
                EMPTY(Nop);
                EMPTY(PirCopy);
#undef EMPTY

            case Tag::IsObject: {
                cs << BC::isobj();
                break;
            }

            case Tag::IsEnvStub: {
                cs << BC::isstubenv();
                break;
            }

#define SIMPLE(Name, Factory)                                                  \
    case Tag::Name: {                                                          \
        cs << BC::Factory();                                                   \
        break;                                                                 \
    }
                SIMPLE(LdFunctionEnv, getEnv);
                SIMPLE(Visible, visible);
                SIMPLE(Invisible, invisible);
                SIMPLE(Identical, identicalNoforce);
                SIMPLE(LOr, lglOr);
                SIMPLE(LAnd, lglAnd);
                SIMPLE(Inc, inc);
                SIMPLE(Force, force);
                SIMPLE(AsTest, asbool);
                SIMPLE(Length, length);
                SIMPLE(ChkMissing, checkMissing);
                SIMPLE(ChkClosure, isfun);
                SIMPLE(Seq, seq);
                SIMPLE(MkCls, close);
                SIMPLE(SetShared, setShared);
                SIMPLE(EnsureNamed, ensureNamed);
#define V(V, name, Name) SIMPLE(Name, name);
                SIMPLE_INSTRUCTIONS(V, _);
#undef V
#undef SIMPLE

#define SIMPLE_WITH_SRCIDX(Name, Factory)                                      \
    case Tag::Name: {                                                          \
        cs << BC::Factory();                                                   \
        cs.addSrcIdx(instr->srcIdx);                                           \
        break;                                                                 \
    }
                SIMPLE_WITH_SRCIDX(Add, add);
                SIMPLE_WITH_SRCIDX(Sub, sub);
                SIMPLE_WITH_SRCIDX(Mul, mul);
                SIMPLE_WITH_SRCIDX(Div, div);
                SIMPLE_WITH_SRCIDX(IDiv, idiv);
                SIMPLE_WITH_SRCIDX(Mod, mod);
                SIMPLE_WITH_SRCIDX(Pow, pow);
                SIMPLE_WITH_SRCIDX(Lt, lt);
                SIMPLE_WITH_SRCIDX(Gt, gt);
                SIMPLE_WITH_SRCIDX(Lte, ge);
                SIMPLE_WITH_SRCIDX(Gte, le);
                SIMPLE_WITH_SRCIDX(Eq, eq);
                SIMPLE_WITH_SRCIDX(Neq, ne);
                SIMPLE_WITH_SRCIDX(Colon, colon);
                SIMPLE_WITH_SRCIDX(AsLogical, asLogical);
                SIMPLE_WITH_SRCIDX(Plus, uplus);
                SIMPLE_WITH_SRCIDX(Minus, uminus);
                SIMPLE_WITH_SRCIDX(Not, not_);
                SIMPLE_WITH_SRCIDX(Extract1_1D, extract1_1);
                SIMPLE_WITH_SRCIDX(Extract2_1D, extract2_1);
                SIMPLE_WITH_SRCIDX(Extract1_2D, extract1_2);
                SIMPLE_WITH_SRCIDX(Extract2_2D, extract2_2);
                SIMPLE_WITH_SRCIDX(Subassign1_1D, subassign1_1);
                SIMPLE_WITH_SRCIDX(Subassign2_1D, subassign2_1);
                SIMPLE_WITH_SRCIDX(Subassign1_2D, subassign1_2);
                SIMPLE_WITH_SRCIDX(Subassign2_2D, subassign2_2);
#undef SIMPLE_WITH_SRCIDX

            case Tag::Call: {
                auto call = Call::Cast(instr);
                cs << BC::call(call->nCallArgs(), Pool::get(call->srcIdx),
                               call->inferAvailableAssumptions());
                break;
            }

            case Tag::NamedCall: {
                auto call = NamedCall::Cast(instr);
                cs << BC::call(call->nCallArgs(), call->names,
                               Pool::get(call->srcIdx),
                               call->inferAvailableAssumptions());
                break;
            }

            case Tag::StaticCall: {
                auto call = StaticCall::Cast(instr);
                SEXP originalClosure = call->cls()->rirClosure();
                auto dt = DispatchTable::unpack(BODY(originalClosure));
                if (auto trg = call->tryOptimisticDispatch()) {
                    // Avoid recursivly compiling the same closure
                    auto fun = compiler.alreadyCompiled(trg);
                    SEXP funCont = nullptr;

                    if (fun) {
                        funCont = fun->container();
                    } else if (!compiler.isCompiling(trg)) {
                        fun = compiler.compile(trg, dryRun);
                        funCont = fun->container();
                        Protect p(funCont);
                        assert(originalClosure &&
                               "Cannot compile synthetic closure");
                        dt->insert(fun);
                    }
                    auto bc = BC::staticCall(call->nCallArgs(),
                                             Pool::get(call->srcIdx),
                                             originalClosure, funCont,
                                             call->inferAvailableAssumptions());
                    cs << bc;
                    if (!funCont)
                        compiler.needsPatching(
                            trg, bc.immediate.staticCallFixedArgs.versionHint);
                } else {
                    // Something went wrong with dispatching, let's put the
                    // baseline there
                    cs << BC::staticCall(
                        call->nCallArgs(), Pool::get(call->srcIdx),
                        originalClosure, dt->baseline()->container(),
                        call->inferAvailableAssumptions());
                }
                break;
            }

            case Tag::CallBuiltin: {
                auto blt = CallBuiltin::Cast(instr);
                cs << BC::callBuiltin(blt->nCallArgs(), Pool::get(blt->srcIdx),
                                      blt->blt);
                break;
            }

            case Tag::CallSafeBuiltin: {
                auto blt = CallSafeBuiltin::Cast(instr);
                cs << BC::callBuiltin(blt->nargs(), Pool::get(blt->srcIdx),
                                      blt->blt);
                break;
            }

            case Tag::PushContext: {
                if (!pushContexts.count(instr))
                    pushContexts[instr] = cs.mkLabel();
                cs << BC::pushContext(pushContexts.at(instr));
                break;
            }

            case Tag::PopContext: {
                auto push = PopContext::Cast(instr)->push();
                if (!pushContexts.count(push))
                    pushContexts[push] = cs.mkLabel();
                cs << pushContexts.at(push);
                cs << BC::popContext();
                break;
            }

            case Tag::MkEnv: {
                auto mkenv = MkEnv::Cast(instr);
                bool stub;
                if (mkenv->stub)
                    stub = true;
                else
                    stub = false;
                cs << BC::mkEnv(mkenv->varName, mkenv->context, stub);
                break;
            }

            case Tag::Phi: {
                // Phi functions are no-ops, because after allocation on
                // CSSA form, all arguments and the funcion itself are
                // allocated to the same place
                auto phi = Phi::Cast(instr);
                phi->eachArg([&](BB*, Value* arg) {
                    assert(((alloc.onStack(phi) && alloc.onStack(arg)) ||
                            (alloc[phi] == alloc[arg])) &&
                           "Phi inputs must all be allocated in 1 slot");
                });
                break;
            }

            case Tag::Return: {
                cs << BC::ret();
                // end of this BB, return
                return;
            }

            case Tag::FrameState:
            case Tag::Deopt:
            case Tag::Assume:
            case Tag::Checkpoint: {
                assert(false && "Deopt instructions must be lowered into "
                                "standard branches and scheduled deopt, "
                                "before pir_2_rir");
                break;
            }

            case Tag::ScheduledDeopt: {
                auto deopt = ScheduledDeopt::Cast(instr);

                size_t nframes = deopt->frames.size();

                SEXP store =
                    Rf_allocVector(RAWSXP, sizeof(DeoptMetadata) +
                                               nframes * sizeof(FrameInfo));
                auto m = new (DATAPTR(store)) DeoptMetadata;
                m->numFrames = nframes;

                size_t i = 0;
                // Frames in the ScheduledDeopt are in pir argument order
                // (from left to right). On the other hand frames in the rir
                // deopt_ instruction are in stack order, from tos down.
                for (auto fi = deopt->frames.rbegin();
                     fi != deopt->frames.rend(); fi++)
                    m->frames[i++] = *fi;

                cs << BC::deopt(store);
                // deopt is exit, return
                return;
            }

            // Values, not instructions
            case Tag::Tombstone:
            case Tag::MissingArg:
            case Tag::UnboundValue:
            case Tag::Env:
            case Tag::Nil:
            case Tag::False:
            case Tag::True: {
                break;
            }

            // Dummy sentinel enum item
            case Tag::_UNUSED_: {
                break;
            }
            }

            // Store the result
            if (alloc.sa.dead(instr)) {
                cs << BC::pop();
            } else if (instr->producesRirResult()) {
                if (!alloc.hasSlot(instr)) {
                    cs << BC::pop();
                } else if (!alloc.onStack(instr)) {
                    cs << BC::stloc(alloc[instr]);
                }
            }
        }

        // This BB has exactly one successor, trueBranch().
        // Jump through empty blocks
        assert(bb->isJmp());
        auto next = jumpThroughEmpty(bb->trueBranch());
        cs << BC::br(bbLabels[next]);
    });

    return alloc.slots();
}

static bool DEBUG_DEOPTS = getenv("PIR_DEBUG_DEOPTS") &&
                           0 == strncmp("1", getenv("PIR_DEBUG_DEOPTS"), 1);
static bool DEOPT_CHAOS = getenv("PIR_DEOPT_CHAOS") &&
                          0 == strncmp("1", getenv("PIR_DEOPT_CHAOS"), 1);
static bool DEOPT_CHAOS_SEED = getenv("PIR_DEOPT_CHAOS_SEED")
                                   ? atoi(getenv("PIR_DEOPT_CHAOS_SEED"))
                                   : std::random_device()();

static bool coinFlip() {
    static std::mt19937 gen(DEOPT_CHAOS_SEED);
    static std::bernoulli_distribution coin(0.03);
    return coin(gen);
};

void Pir2Rir::lower(Code* code) {

    Visitor::runPostChange(code->entry, [&](BB* bb) {
        auto it = bb->begin();
        while (it != bb->end()) {
            auto next = it + 1;
            if (auto call = CallInstruction::CastCall(*it))
                call->clearFrameState();
            if (auto ldfun = LdFun::Cast(*it)) {
                // The guessed binding in ldfun is just used as a temporary
                // store. If we did not manage to resolve ldfun by now, we
                // have to remove the guess again, since apparently we
                // were not sure it is correct.
                if (ldfun->guessedBinding())
                    ldfun->clearGuessedBinding();
            } else if (auto deopt = Deopt::Cast(*it)) {
                // Lower Deopt instructions + their FrameStates to a
                // ScheduledDeopt.
                auto newDeopt = new ScheduledDeopt();
                newDeopt->consumeFrameStates(deopt);
                bb->replace(it, newDeopt);
            } else if (auto expect = Assume::Cast(*it)) {
                auto condition = expect->condition();
                if (DEOPT_CHAOS && coinFlip()) {
                    condition = expect->assumeTrue ? (Value*)False::instance()
                                                   : (Value*)True::instance();
                }
                std::string debugMessage;
                if (DEBUG_DEOPTS) {
                    std::stringstream dump;
                    debugMessage = "DEOPT, assumption ";
                    expect->condition()->printRef(dump);
                    debugMessage += dump.str();
                    debugMessage += " failed in\n";
                    dump.str("");
                    code->printCode(dump, true);
                    debugMessage += dump.str();
                }
                BBTransform::lowerExpect(
                    code, bb, it, condition, expect->assumeTrue,
                    expect->checkpoint()->bb()->falseBranch(), debugMessage);
                // lowerExpect splits the bb from current position. There
                // remains nothing to process. Breaking seems more robust
                // than trusting the modified iterator.
                break;
            }

            it = next;
        }
    });

    Visitor::run(code->entry, [&](BB* bb) {
        auto it = bb->begin();
        while (it != bb->end()) {
            auto next = it + 1;
            if (FrameState::Cast(*it)) {
                next = bb->remove(it);
            } else if (Checkpoint::Cast(*it)) {
                next = bb->remove(it);
                // Branching removed. Preserve invariant
                bb->next1 = nullptr;
            } else if (MkArg::Cast(*it) && (*it)->unused()) {
                next = bb->remove(it);
            }
            it = next;
        }
    });

    // Lower phi functions - transform into a cfg where all input blocks of
    // all phi functions are their immediate predecessors
    {
        bool done;
        CFG cfg(code);
        do {
            done = true;
            BreadthFirstVisitor::run(code->entry, [&](Instruction* i, BB* bb) {
                // Check if this phi has the desired property
                if (auto phi = Phi::Cast(i)) {
                    bool ok = true;
                    phi->eachArg([&](BB* inputBB, Value*) {
                        if (!cfg.isImmediatePredecessor(inputBB, phi->bb()))
                            ok = false;
                    });
                    if (ok)
                        return;
                    done = false;

                    // Accumulate new arguments and inputs for the phi
                    std::vector<std::pair<Value*, BB*>> update;

                    // The idea here is to change the semantics of phi functions
                    // from the one in PIR to the more common one. In PIR, phi
                    // input blocks are not necessarily immediate predecessors.
                    // The phi means, take the value associated with the last
                    // visited bb from all the phi's input blocks. What we want
                    // is, take the value associated with the immediate
                    // predecessor block that we just came from. There is a
                    // subtle difference when loops are in play...

                    // We go backward breadth first from the phi's immediate
                    // predecessors. The idea is, to every path we propagate all
                    // the phi input values. If we find a block that creates one
                    // of the inputs, we just update the input block for that
                    // input. If we find a phi that has as argument one of the
                    // inputs, we replace that argument with this phi. If we
                    // find a merge block (ie. more than one immediate
                    // predecessor), we insert a new phi that has as inputs the
                    // inputs of the current one, and we update the current phi
                    // to have the new phi as input.
                    for (auto pred : cfg.immediatePredecessors(phi->bb())) {
                        BreadthFirstVisitor::checkBackward(
                            pred, cfg, [&](BB* bb) {
                                // Check if block is the origin of one of the
                                // phi args
                                bool done = false;
                                phi->eachArg([&](BB*, Value* val) {
                                    assert(val->isInstruction());
                                    if (Instruction::Cast(val)->bb() == bb) {
                                        assert(!done);
                                        update.emplace_back(val, pred);
                                        done = true;
                                    }
                                });
                                if (done)
                                    return false;
                                // Check if there is a phi in this block that
                                // has one of the args the same as an arg to
                                // the phi we are dealing with. If so, this phi
                                // is the source of our phi input
                                // (pretty much the case above but for phis)
                                for (auto i : VisitorHelpers::reverse(*bb)) {
                                    if (auto p = Phi::Cast(i)) {
                                        bool stop = false;
                                        p->eachArg([&](BB*, Value* v1) {
                                            phi->eachArg([&](BB*, Value* v2) {
                                                if (v1 == v2)
                                                    stop = true;
                                            });
                                        });
                                        if (stop) {
                                            update.emplace_back(p, pred);
                                            return false;
                                        }
                                    }
                                }
                                // Insert a new phi into a merge block
                                if (cfg.immediatePredecessors(bb).size() > 1) {
                                    auto newPhi = new Phi;
                                    phi->eachArg([&](BB* b, Value* v) {
                                        assert(v->isInstruction());
                                        if (cfg.isPredecessor(
                                                Instruction::Cast(v)->bb(), bb))
                                            newPhi->addInput(
                                                Instruction::Cast(v)->bb(), v);
                                    });
                                    bb->insert(bb->begin(), newPhi);
                                    update.emplace_back(newPhi, pred);
                                    return false;
                                }
                                return true;
                            });
                    }
                    // Replace the current phi's args and inputs
                    std::unordered_set<BB*> remove;
                    phi->eachArg([&](BB* bb, Value*) { remove.insert(bb); });
                    phi->removeInputs(remove);
                    for (auto u : update) {
                        phi->addInput(u.second, u.first);
                    }
                }
            });
        } while (!done);
    }

    // Insert Nop into all empty blocks to make life easier
    Visitor::run(code->entry, [&](BB* bb) {
        if (bb->isEmpty())
            bb->append(new Nop());
    });
}

void Pir2Rir::toCSSA(Code* code) {
    // For each Phi, insert copies
    BreadthFirstVisitor::run(code->entry, [&](BB* bb) {
        // TODO: move all phi's to the beginning, then insert the copies not
        // after each phi but after all phi's?
        for (auto it = bb->begin(); it != bb->end(); ++it) {
            auto instr = *it;
            if (auto phi = Phi::Cast(instr)) {
                for (size_t i = 0; i < phi->nargs(); ++i) {
                    BB* pred = phi->inputAt(i);
                    // If pred is branch insert a new split block
                    if (!pred->isJmp()) {
                        BB* split = nullptr;
                        if (pred->trueBranch() == phi->bb())
                            split = pred->trueBranch();
                        else if (pred->falseBranch() == phi->bb())
                            split = pred->falseBranch();
                        assert(split &&
                               "Don't know where to insert a phi input copy.");
                        pred = BBTransform::splitEdge(code->nextBBId++, pred,
                                                      split, code);
                    }
                    if (Instruction* iav =
                            Instruction::Cast(phi->arg(i).val())) {
                        auto copy = pred->insert(pred->end(), new PirCopy(iav));
                        phi->arg(i).val() = *copy;
                    } else {
                        auto val = phi->arg(i).val()->asRValue();
                        auto copy = pred->insert(pred->end(), new LdConst(val));
                        phi->arg(i).val() = *copy;
                    }
                }
                auto phiCopy = new PirCopy(phi);
                phi->replaceUsesWith(phiCopy);
                it = bb->insert(it + 1, phiCopy);
            }
        }
    });
}

rir::Code* Pir2Rir::getPromise(Context& ctx, Promise* p) {
    if (!promises.count(p)) {
        ctx.push(src_pool_at(globalContext(), p->srcPoolIdx()));
        size_t localsCnt = compileCode(ctx, p);
        promises[p] = ctx.finalizeCode(localsCnt);
    }
    return promises.at(p);
}

rir::Function* Pir2Rir::finalize() {
    // TODO: keep track of source ast indices in the source pool
    // (for now, calls, promises and operators do)
    // + how to deal with inlined stuff?

    FunctionWriter function;
    Context ctx(function);

    FunctionSignature signature(FunctionSignature::Environment::CalleeCreated,
                                FunctionSignature::OptimizationLevel::Optimized,
                                cls->assumptions());

    // PIR does not support default args currently.
    for (size_t i = 0; i < cls->nargs(); ++i) {
        function.addArgWithoutDefault();
        signature.pushDefaultArgument();
    }

    assert(signature.formalNargs() == cls->nargs());
    ctx.push(R_NilValue);
    size_t localsCnt = compileCode(ctx, cls);
    log.finalPIR(cls);
    auto body = ctx.finalizeCode(localsCnt);
    function.finalize(body, signature);
#ifdef ENABLE_SLOWASSERT
    CodeVerifier::verifyFunctionLayout(function.function()->container(),
                                       globalContext());
#endif
    log.finalRIR(function.function());
    return function.function();
}

} // namespace

rir::Function* Pir2RirCompiler::compile(ClosureVersion* cls, bool dryRun) {
    auto& log = logger.get(cls);
    done[cls] = nullptr;
    Pir2Rir pir2rir(*this, cls, dryRun, log);
    auto fun = pir2rir.finalize();
    done[cls] = fun;
    log.flush();
    if (fixup.count(cls)) {
        auto fixups = fixup.find(cls);
        for (auto idx : fixups->second)
            Pool::patch(idx, fun->container());
        fixup.erase(fixups);
    }
    return fun;
}

} // namespace pir
} // namespace rir
