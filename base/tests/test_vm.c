#include "test_common.h"
#define VM_IMPLEMENTATION
#include "vm.h"

int main(void)
{
    Vm vm;

    print_cstr("test_vm\n");

    /* (2+3)*4 == 20, plain i64 arithmetic */
    {
        Vm_Instr prog[6];
        i32 rc;

        prog[0].op = VM_OP_IPUSH; prog[0].i_operand = 2;
        prog[1].op = VM_OP_IPUSH; prog[1].i_operand = 3;
        prog[2].op = VM_OP_IADD;
        prog[3].op = VM_OP_IPUSH; prog[3].i_operand = 4;
        prog[4].op = VM_OP_IMUL;
        prog[5].op = VM_OP_HALT;

        vm_init(&vm);
        rc = vm_run(&vm, prog, 6);
        TEST_CHECK(rc == VM_ESUCCESS, "(2+3)*4: run succeeds");
        TEST_CHECK(vm.istack_count == 1, "(2+3)*4: exactly one value left on the i64 stack");
        TEST_CHECK(vm.istack[0] == 20, "(2+3)*4: result is 20");
    }

    /* sum 1..5 == 15 via a conditional-jump loop - counter lives on the
     * i64 stack, running sum on the f64 stack (via I2F), so IDUP/JZ never
     * need to reach past a buried value the way a single shared stack
     * would - see vm.h's own comment on why two typed stacks instead of
     * one tagged one. */
    {
        Vm_Instr prog[11];
        i32 rc;

        prog[0].op = VM_OP_IPUSH; prog[0].i_operand = 5;   /* counter = 5 */
        prog[1].op = VM_OP_FPUSH; prog[1].f_operand = 0.0; /* sum = 0.0 */
        prog[2].op = VM_OP_IDUP;                            /* LOOP: */
        prog[3].op = VM_OP_JZ;    prog[3].i_operand = 10;   /* -> END */
        prog[4].op = VM_OP_IDUP;
        prog[5].op = VM_OP_I2F;
        prog[6].op = VM_OP_FADD;
        prog[7].op = VM_OP_IPUSH; prog[7].i_operand = 1;
        prog[8].op = VM_OP_ISUB;
        prog[9].op = VM_OP_JMP;   prog[9].i_operand = 2;    /* -> LOOP */
        prog[10].op = VM_OP_HALT;                           /* END: */

        vm_init(&vm);
        rc = vm_run(&vm, prog, 11);
        TEST_CHECK(rc == VM_ESUCCESS, "sum 1..5: run succeeds");
        TEST_CHECK(vm.fstack_count == 1, "sum 1..5: exactly one value left on the f64 stack");
        TEST_CHECK(vm.fstack[0] == 15.0, "sum 1..5: result is 15.0");
        TEST_CHECK(vm.istack_count == 1 && vm.istack[0] == 0, "sum 1..5: counter correctly reached 0");
    }

    /* error paths: stack underflow, bad jump target, division by zero */
    {
        Vm_Instr prog[2];
        i32 rc;

        prog[0].op = VM_OP_IADD; /* nothing pushed yet - underflow */
        prog[1].op = VM_OP_HALT;
        vm_init(&vm);
        rc = vm_run(&vm, prog, 2);
        TEST_CHECK(rc == VM_EISTACK_UNDER, "IADD on an empty stack reports underflow, not garbage/crash");
    }
    {
        Vm_Instr prog[1];
        i32 rc;

        prog[0].op = VM_OP_JMP; prog[0].i_operand = 999; /* out of range */
        vm_init(&vm);
        rc = vm_run(&vm, prog, 1);
        TEST_CHECK(rc == VM_EBAD_JUMP, "jump target past the end of the program is rejected");
    }
    {
        Vm_Instr prog[4];
        i32 rc;

        prog[0].op = VM_OP_IPUSH; prog[0].i_operand = 10;
        prog[1].op = VM_OP_IPUSH; prog[1].i_operand = 0;
        prog[2].op = VM_OP_IDIV;
        prog[3].op = VM_OP_HALT;
        vm_init(&vm);
        rc = vm_run(&vm, prog, 4);
        TEST_CHECK(rc == VM_EDIV_ZERO, "10/0 (integer) is rejected, not UB");
    }

    /* ISWAP/FSWAP round-trip */
    {
        Vm_Instr prog[6];
        i32 rc;

        prog[0].op = VM_OP_IPUSH; prog[0].i_operand = 1;
        prog[1].op = VM_OP_IPUSH; prog[1].i_operand = 2;
        prog[2].op = VM_OP_ISWAP; /* [1,2] -> [2,1] (2 now on bottom, 1 on top) */
        prog[3].op = VM_OP_ISUB; /* pops top(1), next(2), pushes 2-1 = 1 */
        prog[4].op = VM_OP_HALT;
        vm_init(&vm);
        rc = vm_run(&vm, prog, 5);
        TEST_CHECK(rc == VM_ESUCCESS, "ISWAP: run succeeds");
        TEST_CHECK(vm.istack[0] == 1, "ISWAP: [1,2] -> [2,1], then ISUB computes 2-1 = 1");
    }

    print_flush();
    return test_failures_ != 0;
}
