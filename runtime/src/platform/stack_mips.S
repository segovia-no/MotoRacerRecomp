# call_on_stack(stack_top, fn): run fn on another stack and come back (PSP / MIPS o32/eabi).
    .set noreorder
    .text
    .globl call_on_stack
    .ent call_on_stack
call_on_stack:
    addiu $a0, $a0, -16
    sw    $sp, 0($a0)          # old stack pointer
    sw    $ra, 4($a0)          # return address
    move  $sp, $a0
    jalr  $a1
    nop
    lw    $ra, 4($sp)
    lw    $sp, 0($sp)
    jr    $ra
    nop
    .end call_on_stack
