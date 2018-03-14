#include "stack_machine.h"
#include "ir/BC.h"
#include "../translations/rir_2_pir.h"
#include "R/Funtab.h"
#include "../util/builder.h"
#include "../pir/pir_impl.h"
#include "../analysis/query.h"

namespace rir {
namespace pir {

void StackMachine::clear() {
    stack.clear();
    entry = nullptr;
    pc = nullptr;
}

size_t StackMachine::stack_size() { return stack.size(); }

Value* StackMachine::pop() {
    assert(stack_size() > 0);
    auto v = stack.back();
    stack.pop_back();
    return v;
}

Value* StackMachine::top() { return stack.back(); }
Value* StackMachine::front() { return stack.front(); }
void StackMachine::push(Value* v) { stack.push_back(v); }

bool StackMachine::empty() { return stack.empty(); }

Value* StackMachine::at(size_t index) {
    assert(index < stack_size());
    return stack[stack_size() - index - 1];
}

void StackMachine::set(size_t index, Value* value) {
    assert(index < stack_size());
    stack[stack_size() - index - 1] = value;
}

void StackMachine::run(BC bc, Builder* builder, rir::Function* src, std::vector<ReturnSite>* results) {
    Value* v;
    Value* x;
    Value* y;
    switch (bc.bc) {
        case Opcode::push_:
            push((*builder)(new LdConst(bc.immediateConst())));
            break;
        case Opcode::ldvar_:
            v = (*builder)(new LdVar(bc.immediateConst(), builder->env));
            push((*builder)(new Force(v)));
            break;
        case Opcode::stvar_:
            v = pop();
            (*builder)(new StVar(bc.immediateConst(), v, builder->env));
            break;
        case Opcode::ldvar2_:
            (*builder)(new LdVarSuper(bc.immediateConst(), builder->env));
            break;
        case Opcode::stvar2_:
            v = pop();
            (*builder)(new StVarSuper(bc.immediateConst(), v, builder->env));
            break;
        case Opcode::ret_:
            results->push_back(ReturnSite(builder->bb, pop()));
            assert(empty());
            break;
        case Opcode::asbool_:
        case Opcode::aslogical_:
            push((*builder)(new AsLogical(pop())));
            break;
        case Opcode::ldfun_:
            push((*builder)(new LdFun(bc.immediateConst(), builder->env)));
            break;
        case Opcode::guard_fun_:
            std::cout << "warn: guard ignored "
                        << CHAR(PRINTNAME(
                                rir::Pool::get(bc.immediate.guard_fun_args.name)))
                        << "\n";
            break;
        case Opcode::swap_:
            x = pop();
            y = pop();
            push(x);
            push(y);
            break;
        case Opcode::dup_:
            push(top());
            break;
        case Opcode::dup2_:
            x = at(0);
            y = at(1);
            push(y);
            push(x);
            break;
        case Opcode::pull_: {
            size_t i = bc.immediate.i;
            push(at(i));
            break;
        }
        case Opcode::close_: {
            Value* x = pop();
            Value* y = pop();
            Value* z = pop();
            push((*builder)(new MkCls(x, y, z, builder->env)));
            break;
        }
        case Opcode::nop_:
            break;
        case Opcode::pop_:
            pop();
            break;
        case Opcode::call_: {
            unsigned n = bc.immediate.call_args.nargs;
            rir::CallSite* cs = bc.callSite(src->body());

            std::vector<Value*> args;
            for (size_t i = 0; i < n; ++i) {
                unsigned argi = cs->args()[i];
                if (argi == DOTS_ARG_IDX) {
                    assert(false);
                } else if (argi == MISSING_ARG_IDX) {
                    assert(false);
                }
                Promise* prom = builder->function->createProm();
                {
                    // What should I do with this?
                    Builder pb(builder->function, prom);
                    Rir2Pir compiler = Rir2Pir();
                    IRTransformation* rir2Pir = compiler.declare(src);
                    compiler.compileFunction(rir2Pir, true);
                    //CodeCompiler c(pb, src, code, cmp);
                    //c(true);
                }
                Value* val = Missing::instance();
                if (Query::pure(prom)) {
                    Rir2Pir compiler = Rir2Pir();
                    IRTransformation* rir2Pir = compiler.declare(src);
                    compiler.compileFunction(rir2Pir, false);
                    //val = c(false);
                }
                args.push_back((*builder)(new MkArg(prom, val, builder->env)));
            }

            push((*builder)(new Call(builder->env, pop(), args)));
            break;
        }
        case Opcode::promise_: {
            //unsigned promi = bc.immediate.i;
            Promise* prom = builder->function->createProm();
            {
                // What should I do with this?
                Builder pb(builder->function, prom);
                Rir2Pir compiler = Rir2Pir();
                compiler.compileFunction(src);
                //CodeCompiler c(pb, src, code, cmp);
                //c(true);
            }
            Value* val = Missing::instance();
            if (Query::pure(prom)) {
                Rir2Pir compiler = Rir2Pir();
                compiler.compileFunction(src);
                //val = c(false);
            }
            // TODO: Remove comment and check how to deal with
            push((*builder)(new MkArg(prom, val, builder->env)));
            break;
        }
        case Opcode::static_call_stack_: {
            unsigned n = bc.immediate.call_args.nargs;
            rir::CallSite* cs = bc.callSite(src->body());
            SEXP target = rir::Pool::get(*cs->target());

            std::vector<Value*> args(n);
            for (size_t i = 0; i < n; ++i)
                args[n - i - 1] = pop();

            // TODO: compile a list of safe builtins
            static int vector = findBuiltin("vector");

            if (getBuiltinNr(target) == vector)
                push((*builder)(new CallSafeBuiltin(target, args)));
            else
                push((*builder)(new CallBuiltin(builder->env, target, args)));
            break;
        }
        case Opcode::seq_: {
            auto x = pop();
            auto y = pop();
            auto z = pop();
            push((*builder)(new Seq(x, y, z)));
            break;
        }
        case Opcode::for_seq_size_:
            push((*builder)(new ForSeqSize(top())));
            break;

        case Opcode::extract1_1_: {
            Value* vec = pop();
            Value* idx = pop();
            push((*builder)(new Extract1_1D(vec, idx)));
            break;
        }

        case Opcode::extract2_1_: {
            Value* vec = pop();
            Value* idx = pop();
            push((*builder)(new Extract2_1D(vec, idx)));
            break;
        }

        case Opcode::extract1_2_: {
            Value* vec = pop();
            Value* idx1 = pop();
            Value* idx2 = pop();
            push((*builder)(new Extract1_2D(vec, idx1, idx2)));
            break;
        }

        case Opcode::extract2_2_: {
            Value* vec = pop();
            Value* idx1 = pop();
            Value* idx2 = pop();
            push((*builder)(new Extract2_2D(vec, idx1, idx2)));
            break;
        }

        case Opcode::subassign1_: {
            Value* vec = pop();
            Value* idx = pop();
            Value* val = pop();
            push((*builder)(new Subassign1_1D(vec, idx, val)));
            break;
        }

        case Opcode::subassign2_: {
            Value* vec = pop();
            Value* idx = pop();
            Value* val = pop();
            push((*builder)(new Subassign2_1D(vec, idx, val)));
            break;
        }

        #define BINOP(Name, Op)                                                        \
        case Opcode::Op:                                                           \
            x = pop();                                                             \
            y = pop();                                                             \
            push((*builder)(new Name(x, y)));                                               \
            break
                BINOP(LOr, lgl_or_);
                BINOP(LAnd, lgl_and_);
                BINOP(Lt, lt_);
                BINOP(Gt, gt_);
                BINOP(Gte, le_);
                BINOP(Lte, ge_);
                BINOP(Mod, mod_);
                BINOP(Div, div_);
                BINOP(IDiv, idiv_);
                BINOP(Add, add_);
                BINOP(Mul, mul_);
                BINOP(Colon, colon_);
                BINOP(Pow, pow_);
                BINOP(Sub, sub_);
                BINOP(Eq, eq_);
                BINOP(Neq, ne_);

        #undef BINOP
        #define UNOP(Name, Op)                                                         \
        case Opcode::Op:                                                           \
            v = pop();                                                             \
            push((*builder)(new Name(v)));                                                  \
            break
                UNOP(Plus, uplus_);
                UNOP(Minus, uminus_);
                UNOP(Inc, inc_);
                UNOP(Not, not_);
                UNOP(Is, is_);
                UNOP(Length, length_);
        #undef UNOP

        case Opcode::pick_:
            push(at(bc.immediate.i));
            break;

        case Opcode::put_:
            x = top();
            for (size_t i = 0; i < bc.immediate.i - 1; ++i)
                set(i, at(i + 1));
            set(bc.immediate.i, x);
            break;

        // TODO implement!
        // (silently ignored)
        case Opcode::set_shared_:
        case Opcode::invisible_:
        case Opcode::visible_:
        case Opcode::isfun_:
        case Opcode::make_unique_:
        case Opcode::brobj_:
            break;

        // Currently unused opcodes:
        case Opcode::ldarg_:
        case Opcode::alloc_:
        case Opcode::push_code_:
        case Opcode::set_names_:
        case Opcode::names_:
        case Opcode::force_:

        // Invalid opcodes:
        case Opcode::label:
        case Opcode::invalid_:
        case Opcode::num_of:

        // Opcodes handled elsewhere
        case Opcode::brtrue_:
        case Opcode::brfalse_:
        case Opcode::br_:
            assert(false);

        // Unsupported opcodes:
        case Opcode::ldloc_:
        case Opcode::stloc_:
        case Opcode::ldlval_:
        case Opcode::asast_:
        case Opcode::missing_:
        case Opcode::dispatch_stack_:
        case Opcode::dispatch_:
        case Opcode::guard_env_:
        case Opcode::call_stack_:
        case Opcode::return_:
        case Opcode::beginloop_:
        case Opcode::endcontext_:
        case Opcode::ldddvar_:
        case Opcode::int3_:
            std::cerr << "Cannot compile Function. Unsupported bc\n";
            bc.print();
            assert(false);
            break;
    }
}

bool StackMachine::doMerge(Opcode* trg, Builder* builder, StackMachine* other) {
    if (other->entry == nullptr) {
        other->entry = builder->createBB();
        other->pc = trg;
        for (size_t i = 0; i < stack_size(); ++i) {
            auto v = stack.at(i);
            auto p = new Phi;
            other->entry->append(p);
            p->addInput(builder->bb, v);
            stack.push_back(p);
        }

        return true;
    }

    assert(stack_size() == other->stack_size());

    for (size_t i = 0; i < stack_size(); ++i) {
        Phi* p = Phi::Cast(stack.at(i));
        assert(p);
        Value* incom = stack.at(i);
        if (incom != p) {
            p->addInput(builder->bb, incom);
        }
    }
    return false;
}

Opcode* StackMachine::getPC() { return pc; }
void StackMachine::setPC(Opcode* opcode) { pc = opcode;}
pir::BB* StackMachine::getEntry() { return entry; }
void StackMachine::setEntry(pir::BB* ent) {entry = ent; }
}
}