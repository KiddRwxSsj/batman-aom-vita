@ stand-ins for the linux kuser helpers at 0xffff0fc0 / 0xffff0fa0 that libgcc's
@ __sync_* code in the .so calls directly. same contract: only r0, r3, ip and flags change

    .syntax unified
    .arm
    .text

@ r0 = old, r1 = new, r2 = ptr. r0 = 0 and C set if *ptr was swapped
    .global kuser_cmpxchg
    .type kuser_cmpxchg, %function
kuser_cmpxchg:
    dmb
1:  ldrex r3, [r2]
    subs r3, r3, r0
    bne 2f
    strex r3, r1, [r2]
    teq r3, #1
    beq 1b
    dmb
    rsbs r0, r3, #0
    bx lr
2:  dmb
    mov r0, #1
    cmp r0, #2
    bx lr

    .global kuser_memory_barrier
    .type kuser_memory_barrier, %function
kuser_memory_barrier:
    dmb
    bx lr
