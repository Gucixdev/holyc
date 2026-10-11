#ifndef HOLYC_VM_H_
#define HOLYC_VM_H_

/* A small stack-based bytecode virtual machine - scope stated plainly
 * up front, because "vm.h" as a name could reasonably suggest something
 * far bigger (a general-purpose language runtime, a JIT, a garbage
 * collector). This is deliberately none of that: a fixed, minimal
 * instruction set (i64/f64 arithmetic, stack manipulation, comparison,
 * unconditional/conditional jump, halt) that a caller hand-assembles
 * into a flat instruction array. There is no assembler, no compiler,
 * no bytecode *format* beyond "an array of Vm_Instr you built by hand
 * or with your own small generator" - that flat array IS the "language"
 * here, nothing higher-level than that. Same honest-scope-cut house
 * style as arena_alloc.h's fp64_mul/fp64_div comment or bit.h's own
 * "not implemented here" list: this is a real, useful, bounded tool
 * (embed a tiny scriptable rule/condition evaluator without pulling in
 * a whole language), not a shrunk-down promise of a bigger one.
 *
 * Two typed stacks (i64 and f64) rather than one tagged stack - a
 * tagged union stack needs a runtime type check (and a branch) on every
 * single op; this project's branchless-leaning stance
 * makes "the caller picks the right typed op for their data" the
 * better tradeoff than "the VM figures out the type at runtime". A
 * caller mixing i64 and f64 in one program converts explicitly via
 * VM_OP_I2F/VM_OP_F2I, the same way C itself requires an explicit cast. */

#include "base.h"
#include "arena_alloc.h"

typedef enum {
    VM_OP_HALT = 0,

    /* i64 stack: push a constant (operand = the i64 value, stored in
     * the instruction itself), arithmetic, dup/pop */
    VM_OP_IPUSH,
    VM_OP_IADD,
    VM_OP_ISUB,
    VM_OP_IMUL,
    VM_OP_IDIV,
    VM_OP_IDUP,
    VM_OP_IPOP,
    VM_OP_ISWAP, /* swap top two i64 stack entries - needed for anything
                    that keeps more than one live value across a loop
                    iteration (e.g. an accumulator below a loop counter) */

    /* f64 stack: same shape as the i64 ops above, operand is an f64
     * constant instead */
    VM_OP_FPUSH,
    VM_OP_FADD,
    VM_OP_FSUB,
    VM_OP_FMUL,
    VM_OP_FDIV,
    VM_OP_FDUP,
    VM_OP_FPOP,
    VM_OP_FSWAP,

    /* comparison: pops 2 i64, pushes 1 i64 (1 or 0) back onto the i64
     * stack - a boolean result is naturally an integer, not a new
     * third stack */
    VM_OP_IEQ,
    VM_OP_ILT,
    VM_OP_IGT,

    /* control flow: operand is an absolute instruction index (not a
     * relative offset - simpler to hand-assemble by hand, matches how
     * a caller would most naturally write out a jump table). JZ pops
     * one i64 off the i64 stack and jumps if it's 0. */
    VM_OP_JMP,
    VM_OP_JZ,

    /* conversion */
    VM_OP_I2F, /* pop i64, push its f64 equivalent */
    VM_OP_F2I  /* pop f64, push its i64 truncation */
} Vm_Opcode;

typedef struct {
    Vm_Opcode op;
    i64 i_operand; /* used by IPUSH, JMP, JZ (as the jump target) */
    f64 f_operand; /* used by FPUSH */
} Vm_Instr;

#ifndef VM_STACK_CAPACITY
#define VM_STACK_CAPACITY 256
#endif

typedef enum {
    VM_ESUCCESS     = 0,
    VM_EISTACK_OVER = -1, /* i64 stack overflow */
    VM_EISTACK_UNDER= -2, /* i64 stack underflow */
    VM_EFSTACK_OVER = -3, /* f64 stack overflow */
    VM_EFSTACK_UNDER= -4, /* f64 stack underflow */
    VM_EBAD_JUMP    = -5, /* jump target outside the program */
    VM_EBAD_OP      = -6, /* unrecognized opcode value */
    VM_EDIV_ZERO    = -7  /* IDIV by zero (FDIV by zero is not an error -
                              IEEE754 gives inf/nan, same as plain C) */
} Vm_Error;

typedef struct {
    i64 istack[VM_STACK_CAPACITY];
    size_t istack_count;
    f64 fstack[VM_STACK_CAPACITY];
    size_t fstack_count;
} Vm;

void vm_init(Vm *vm);

/* runs from instruction 0 until VM_OP_HALT or an error. Returns
 * VM_ESUCCESS (having hit HALT) or the first error encountered - a
 * caller that wants the final result reads vm->istack/vm->fstack
 * directly afterward (top of stack is istack[istack_count-1] /
 * fstack[fstack_count-1]). */
i32 vm_run(Vm *vm, const Vm_Instr *program, size_t program_count);

#endif /* HOLYC_VM_H_ */

#ifdef VM_IMPLEMENTATION
#ifndef VM_IMPLEMENTATION_
#define VM_IMPLEMENTATION_

void vm_init(Vm *vm)
{
    vm->istack_count = 0;
    vm->fstack_count = 0;
}

static i32 vm_ipush_(Vm *vm, i64 v)
{
    if (vm->istack_count >= VM_STACK_CAPACITY) return VM_EISTACK_OVER;
    vm->istack[vm->istack_count++] = v;
    return VM_ESUCCESS;
}

static i32 vm_ipop_(Vm *vm, i64 *out)
{
    if (vm->istack_count == 0) return VM_EISTACK_UNDER;
    *out = vm->istack[--vm->istack_count];
    return VM_ESUCCESS;
}

static i32 vm_fpush_(Vm *vm, f64 v)
{
    if (vm->fstack_count >= VM_STACK_CAPACITY) return VM_EFSTACK_OVER;
    vm->fstack[vm->fstack_count++] = v;
    return VM_ESUCCESS;
}

static i32 vm_fpop_(Vm *vm, f64 *out)
{
    if (vm->fstack_count == 0) return VM_EFSTACK_UNDER;
    *out = vm->fstack[--vm->fstack_count];
    return VM_ESUCCESS;
}

i32 vm_run(Vm *vm, const Vm_Instr *program, size_t program_count)
{
    size_t pc = 0;

    while (pc < program_count) {
        const Vm_Instr *ins = &program[pc];
        i32 err = VM_ESUCCESS;
        i64 ia, ib;
        f64 fa, fb;

        switch (ins->op) {
        case VM_OP_HALT:
            return VM_ESUCCESS;

        case VM_OP_IPUSH:
            err = vm_ipush_(vm, ins->i_operand);
            break;
        case VM_OP_IADD:
            if ((err = vm_ipop_(vm, &ib)) == VM_ESUCCESS && (err = vm_ipop_(vm, &ia)) == VM_ESUCCESS)
                err = vm_ipush_(vm, ia + ib);
            break;
        case VM_OP_ISUB:
            if ((err = vm_ipop_(vm, &ib)) == VM_ESUCCESS && (err = vm_ipop_(vm, &ia)) == VM_ESUCCESS)
                err = vm_ipush_(vm, ia - ib);
            break;
        case VM_OP_IMUL:
            if ((err = vm_ipop_(vm, &ib)) == VM_ESUCCESS && (err = vm_ipop_(vm, &ia)) == VM_ESUCCESS)
                err = vm_ipush_(vm, ia * ib);
            break;
        case VM_OP_IDIV:
            if ((err = vm_ipop_(vm, &ib)) == VM_ESUCCESS && (err = vm_ipop_(vm, &ia)) == VM_ESUCCESS) {
                if (ib == 0) return VM_EDIV_ZERO;
                err = vm_ipush_(vm, ia / ib);
            }
            break;
        case VM_OP_IDUP:
            if ((err = vm_ipop_(vm, &ia)) == VM_ESUCCESS) {
                if ((err = vm_ipush_(vm, ia)) == VM_ESUCCESS)
                    err = vm_ipush_(vm, ia);
            }
            break;
        case VM_OP_IPOP:
            err = vm_ipop_(vm, &ia);
            break;
        case VM_OP_ISWAP:
            if ((err = vm_ipop_(vm, &ib)) == VM_ESUCCESS && (err = vm_ipop_(vm, &ia)) == VM_ESUCCESS) {
                if ((err = vm_ipush_(vm, ib)) == VM_ESUCCESS)
                    err = vm_ipush_(vm, ia);
            }
            break;

        case VM_OP_FPUSH:
            err = vm_fpush_(vm, ins->f_operand);
            break;
        case VM_OP_FADD:
            if ((err = vm_fpop_(vm, &fb)) == VM_ESUCCESS && (err = vm_fpop_(vm, &fa)) == VM_ESUCCESS)
                err = vm_fpush_(vm, fa + fb);
            break;
        case VM_OP_FSUB:
            if ((err = vm_fpop_(vm, &fb)) == VM_ESUCCESS && (err = vm_fpop_(vm, &fa)) == VM_ESUCCESS)
                err = vm_fpush_(vm, fa - fb);
            break;
        case VM_OP_FMUL:
            if ((err = vm_fpop_(vm, &fb)) == VM_ESUCCESS && (err = vm_fpop_(vm, &fa)) == VM_ESUCCESS)
                err = vm_fpush_(vm, fa * fb);
            break;
        case VM_OP_FDIV:
            if ((err = vm_fpop_(vm, &fb)) == VM_ESUCCESS && (err = vm_fpop_(vm, &fa)) == VM_ESUCCESS)
                err = vm_fpush_(vm, fa / fb); /* IEEE754 div-by-zero -> inf/nan, not an error */
            break;
        case VM_OP_FDUP:
            if ((err = vm_fpop_(vm, &fa)) == VM_ESUCCESS) {
                if ((err = vm_fpush_(vm, fa)) == VM_ESUCCESS)
                    err = vm_fpush_(vm, fa);
            }
            break;
        case VM_OP_FPOP:
            err = vm_fpop_(vm, &fa);
            break;
        case VM_OP_FSWAP:
            if ((err = vm_fpop_(vm, &fb)) == VM_ESUCCESS && (err = vm_fpop_(vm, &fa)) == VM_ESUCCESS) {
                if ((err = vm_fpush_(vm, fb)) == VM_ESUCCESS)
                    err = vm_fpush_(vm, fa);
            }
            break;

        case VM_OP_IEQ:
            if ((err = vm_ipop_(vm, &ib)) == VM_ESUCCESS && (err = vm_ipop_(vm, &ia)) == VM_ESUCCESS)
                err = vm_ipush_(vm, ia == ib ? 1 : 0);
            break;
        case VM_OP_ILT:
            if ((err = vm_ipop_(vm, &ib)) == VM_ESUCCESS && (err = vm_ipop_(vm, &ia)) == VM_ESUCCESS)
                err = vm_ipush_(vm, ia < ib ? 1 : 0);
            break;
        case VM_OP_IGT:
            if ((err = vm_ipop_(vm, &ib)) == VM_ESUCCESS && (err = vm_ipop_(vm, &ia)) == VM_ESUCCESS)
                err = vm_ipush_(vm, ia > ib ? 1 : 0);
            break;

        case VM_OP_JMP:
            if (ins->i_operand < 0 || (size_t)ins->i_operand >= program_count) return VM_EBAD_JUMP;
            pc = (size_t)ins->i_operand;
            continue;
        case VM_OP_JZ:
            if ((err = vm_ipop_(vm, &ia)) == VM_ESUCCESS && ia == 0) {
                if (ins->i_operand < 0 || (size_t)ins->i_operand >= program_count) return VM_EBAD_JUMP;
                pc = (size_t)ins->i_operand;
                continue;
            }
            break;

        case VM_OP_I2F:
            if ((err = vm_ipop_(vm, &ia)) == VM_ESUCCESS)
                err = vm_fpush_(vm, (f64)ia);
            break;
        case VM_OP_F2I:
            if ((err = vm_fpop_(vm, &fa)) == VM_ESUCCESS)
                err = vm_ipush_(vm, (i64)fa);
            break;

        default:
            return VM_EBAD_OP;
        }

        if (err != VM_ESUCCESS) return err;
        pc++;
    }

    return VM_ESUCCESS; /* fell off the end without HALT - still a clean run */
}

#endif /* VM_IMPLEMENTATION_ */
#endif /* VM_IMPLEMENTATION */
