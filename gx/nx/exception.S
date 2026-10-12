.macro CODE_BEGIN name
    .section .text.\name, "ax", %progbits
    .global \name
    .type \name, %function
    .align 2
    .cfi_startproc
\name:
.endm

.macro CODE_END
    .cfi_endproc
.endm

// gx: how long the short way through a fault may take, in ticks of the counter
// (19.2 MHz: 52 ns each), before the processor is taken to have been away
// from the thread meanwhile. The way itself is some forty instructions.
#define GX_AWAY_TICKS 12

// gx: the kernel may take the processor away again, said where the short way
// through a fault ends. While it was not to (a count the kernel reads in the
// thread's local storage, see the entry), it may have wanted to and "pinned"
// the thread instead, which it says in the flag next to the count: a pinned
// thread may make no call but the one that says it is done, made here, with
// what that call clears kept in what the thread keeps (x4). Leaves x0, x1, x4
// and x9-x30 as they were.
.macro GX_PREEMPTIBLE
    mrs x5, tpidrro_el0
    strh wzr, [x5, #0x100]
    ldrh w6, [x5, #0x102]
    cbz w6, 90f
    stp x0, x1, [x4, #240]
    stp x9, x10, [x4, #128]
    stp x11, x12, [x4, #144]
    stp x13, x14, [x4, #160]
    stp x15, x16, [x4, #176]
    str x17, [x4, #192]
    svc 0x36                    // svcSynchronizePreemptionState
    adrp x5, gx_fault_slot
    ldr w5, [x5, #:lo12:gx_fault_slot]
    mrs x4, tpidrro_el0
    ldr x4, [x4, x5]
    sub x4, x4, #1024
    ldp x0, x1, [x4, #240]
    ldp x9, x10, [x4, #128]
    ldp x11, x12, [x4, #144]
    ldp x13, x14, [x4, #160]
    ldp x15, x16, [x4, #176]
    ldr x17, [x4, #192]
    adrp x5, gx_fault_pinned
    ldr x6, [x5, #:lo12:gx_fault_pinned]
    add x6, x6, #1
    str x6, [x5, #:lo12:gx_fault_pinned]
90:
.endm

// gx: this is libnx's nx/source/runtime/exception.s (ISC license, copyright
// the libnx authors), with one addition at the top of the entry: reads of the
// virtual counter are emulated. Horizon lets programs read the physical
// counter (cntpct_el0) but traps cntvct_el0, which Linux programs read freely;
// Chromium does for its clocks. The two count the same thing here.

// Called by crt0 when the args at the time of entry indicate an exception occured.

.weak __libnx_exception_handler

.weak __libnx_exception_entry
CODE_BEGIN __libnx_exception_entry
    cmp x1, #0
    beq __libnx_exception_entry_abort

    // gx, the short way, for a thread that runs the guest. From here to the
    // call that ends the fault, the kernel does not keep this thread's
    // vector registers should it take the processor away (it saves only
    // those a function must keep, and gives back old ones for the rest:
    // KScheduler's test of "in a user exception handler" is the wrong way
    // round), and a guest that faults by writing to memory for the first
    // time is as a rule in the middle of copying something through them:
    // a Steam client's scripts and strings came out with pieces of older
    // copies in them. So this is kept to as few instructions as will do:
    // where the thread keeps what a fault needs is in a slot of its local
    // storage (host_nx.c, fault_stack()), not looked for in a table, and
    // the way back, which has all the registers in memory to go by, is
    // begun again when it took long enough for the processor to have been
    // away (.Lgx_back). On the way in nothing can be done but count.
    adrp x5, gx_fault_slot
    ldr w5, [x5, #:lo12:gx_fault_slot]
    cbz w5, .Lgx_slow
    mrs x6, tpidrro_el0
    ldr x4, [x6, x5]
    cbz x4, .Lgx_slow           // not a thread that runs the guest
    // And for those few instructions the kernel is asked to leave the
    // processor with this thread: with this count set it does, against the
    // other threads of the program, which are nearly all there is to take
    // it away. GX_PREEMPTIBLE ends it, on every way out of here.
    mov w5, #1
    strh w5, [x6, #0x100]
    mrs x7, cntpct_el0          // when this began
    sub x4, x4, #1024           // what is kept: see gx_fault_tramp
    ldr w2, [x1, #108]
    lsr w2, w2, #26
    cmp w2, #0x3c               // a break: the end of gx_fault_tramp?
    beq .Lgx_back
    cmp w2, #0x24               // data abort
    beq 30f
    cmp w2, #0x20               // instruction abort
    beq 30f
    cbz w2, 30f                 // an instruction the processor does not have
    cmp w2, #0x18               // a read of the virtual counter?
    bne .Lgx_fast_not_ours
    ldr x5, [x1, #88]
    ldr w3, [x5]
    and w3, w3, #0xffffffe0
    mov w5, #0xe040             // mrs xN, cntvct_el0
    movk w5, #0xd53b, lsl #16
    cmp w3, w5
    bne .Lgx_fast_not_ours
    // It is replaced, by C, with a read of the counter that is not trapped
    // (gmem_counter()): once for each place, and no fault there again.
    // Where that cannot be it comes here a second time, to be done below.
    ldr x5, [x4, #1008]
    cbz x5, 31f
    str xzr, [x4, #1008]
    GX_PREEMPTIBLE
    b .Lgx_slow
30: ldr x5, [x4, #1008]         // one that C could do nothing about, come again?
    cbz x5, 31f
    str xzr, [x4, #1008]
    b .Lgx_fast_not_ours
31: ldr x5, [x4, #1016]         // a fault in the handling of a fault?
    cbnz x5, .Lgx_fast_not_ours
    mov x5, #1
    str x5, [x4, #1016]
    ldp x5, x6, [x1, #0]
    stp x5, x6, [x4, #0]
    ldp x5, x6, [x1, #16]
    stp x5, x6, [x4, #16]
    ldp x5, x6, [x1, #32]
    stp x5, x6, [x4, #32]
    ldp x5, x6, [x1, #48]
    stp x5, x6, [x4, #48]
    ldp x5, x6, [x1, #64]
    stp x5, x6, [x4, #64]
    ldp x5, x6, [x1, #80]
    stp x5, x6, [x4, #80]
    ldp x5, x6, [x1, #96]
    stp x5, x6, [x4, #96]
    ldr x5, [x1, #112]
    str x5, [x4, #112]
    adrp x5, gx_fault_tramp
    add x5, x5, #:lo12:gx_fault_tramp
    str x5, [x1, #88]
    str x4, [x1, #80]
    str x4, [x1, #0]
    mrs x5, cntpct_el0
    sub x5, x5, x7
    cmp x5, #GX_AWAY_TICKS
    blo 32f
    adrp x5, gx_fault_away      // the registers may be gone: counted
    ldr x6, [x5, #:lo12:gx_fault_away]
    add x6, x6, #1
    str x6, [x5, #:lo12:gx_fault_away]
32: GX_PREEMPTIBLE
    mov w0, wzr
    bl svcReturnFromException
    b .
.Lgx_back:
    ldr x5, [x1, #88]
    adrp x6, gx_fault_tramp_end
    add x6, x6, #:lo12:gx_fault_tramp_end
    cmp x5, x6
    bne .Lgx_fast_not_ours
    ldp x5, x6, [x4, #0]        // x0-x8, lr, sp, pc and the flags, as they were
    stp x5, x6, [x1, #0]
    ldp x5, x6, [x4, #16]
    stp x5, x6, [x1, #16]
    ldp x5, x6, [x4, #32]
    stp x5, x6, [x1, #32]
    ldp x5, x6, [x4, #48]
    stp x5, x6, [x1, #48]
    ldp x5, x6, [x4, #64]
    stp x5, x6, [x1, #64]
    ldp x5, x6, [x4, #80]
    stp x5, x6, [x1, #80]
    ldr x5, [x4, #96]
    str x5, [x1, #96]
    mrs x5, cntpct_el0
    sub x5, x5, x7
    cmp x5, #GX_AWAY_TICKS
    bhs 34f
33: GX_PREEMPTIBLE
    mov w0, wzr
    bl svcReturnFromException
    b .
34: // Long enough for the processor to have been away: the vector registers
    // gx_fault_tramp put back may be old ones by now. It puts them back
    // again and comes here again; not for ever, should a clock be wrong.
    ldr x5, [x4, #968]
    cmp x5, #8
    bhs 33b
    add x5, x5, #1
    str x5, [x4, #968]
    adrp x5, gx_fault_again
    ldr x6, [x5, #:lo12:gx_fault_again]
    add x6, x6, #1
    str x6, [x5, #:lo12:gx_fault_again]
    adrp x5, gx_fault_tramp_again
    add x5, x5, #:lo12:gx_fault_tramp_again
    str x5, [x1, #88]
    str x4, [x1, #80]
    b 33b

.Lgx_fast_not_ours:
    GX_PREEMPTIBLE
    b .Lgx_not_ours

.Lgx_slow:
    // x1 is the context the kernel resumes from: x0-x8, lr (72), sp (80),
    // pc (88), pstate, afsr0, afsr1, esr (108), far. Everything else is as
    // the thread left it, so only x0-x8 and x30 may be used here.
    ldr w2, [x1, #108]
    lsr w2, w2, #26
    cmp w2, #0x18               // a trapped system register access?
    bne .Lgx_not_counter
    ldr x2, [x1, #88]
    ldr w3, [x2]
    and w4, w3, #0xffffffe0
    mov w5, #0xe040             // mrs xN, cntvct_el0
    movk w5, #0xd53b, lsl #16
    cmp w4, w5
    bne .Lgx_not_counter
    mrs x4, cntpct_el0
    and x3, x3, #31
    cmp x3, #8
    bhi 1f
    str x4, [x1, x3, lsl #3]    // x0-x8 come back from the context
    b .Lgx_counter_done
1:  cmp x3, #30
    bne 2f
    str x4, [x1, #72]
    b .Lgx_counter_done
2:  cmp x3, #31
    beq .Lgx_counter_done
    adr x5, .Lgx_counter_table
    sub x3, x3, #9
    add x5, x5, x3, lsl #3
    br x5
.Lgx_counter_table:
    .irp n, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29
    mov x\n, x4
    b .Lgx_counter_done
    .endr
.Lgx_counter_done:
    add x2, x2, #4              // on to the next instruction
    str x2, [x1, #88]
    mov w0, wzr
    bl svcReturnFromException
    b .
.Lgx_not_counter:
    // gx, the way faults are handled since Steam: not here. Until the kernel
    // is told the fault is over (svcReturnFromException), it does not keep
    // the vector registers of this thread the way it does otherwise: when it
    // takes the processor away for a moment, only those a function must keep
    // come back, and when it fetches them at their first use, likewise. C
    // that maps memory is long enough for the first, and the guest's own
    // were lost to the second often enough to spoil what a Steam client was
    // copying. So the fault is ended at once, into gx_fault_tramp below,
    // which is ordinary code: it keeps all the registers, has C look at the
    // fault, puts them back and comes here a second time (a break) to be
    // sent where the fault was, with the registers a return cannot carry.
    // What the kernel gave is kept meanwhile at the far end of the thread's
    // own stack (host_nx.c, fault_stack()), found by its local storage.
    ldr w2, [x1, #108]
    lsr w2, w2, #26
    mrs x5, tpidrro_el0
    adrp x6, gx_fault_stacks
    add x6, x6, #:lo12:gx_fault_stacks
    mov x7, #128
21: ldp x8, x4, [x6], #16       // a thread's local storage, and its room
    cmp x8, x5
    beq 22f
    subs x7, x7, #1
    bne 21b
    b .Lgx_old                  // not a thread that runs the guest
22: cbz x4, .Lgx_old
    sub x4, x4, #1024           // what is kept: see gx_fault_tramp
    cmp w2, #0x3c               // a break: the end of gx_fault_tramp?
    bne 23f
    ldr x5, [x1, #88]
    adrp x6, gx_fault_tramp_end
    add x6, x6, #:lo12:gx_fault_tramp_end
    cmp x5, x6
    bne .Lgx_not_ours
    ldp x5, x6, [x4, #0]        // x0-x8, lr, sp, pc and the flags, as they were
    stp x5, x6, [x1, #0]
    ldp x5, x6, [x4, #16]
    stp x5, x6, [x1, #16]
    ldp x5, x6, [x4, #32]
    stp x5, x6, [x1, #32]
    ldp x5, x6, [x4, #48]
    stp x5, x6, [x1, #48]
    ldp x5, x6, [x4, #64]
    stp x5, x6, [x1, #64]
    ldp x5, x6, [x4, #80]
    stp x5, x6, [x1, #80]
    ldr x5, [x4, #96]
    str x5, [x1, #96]
    str xzr, [x4, #1016]
    mov w0, wzr
    bl svcReturnFromException
    b .
23: cmp w2, #0x20               // instruction abort
    beq 24f
    cmp w2, #0x24               // data abort
    beq 24f
    cbnz w2, .Lgx_not_ours      // or an instruction the processor does not have
24: ldr x5, [x4, #1008]         // one that C could do nothing about, come again?
    cbz x5, 25f
    str xzr, [x4, #1008]
    b .Lgx_not_ours
25: ldr x5, [x4, #1016]         // a fault in the handling of a fault?
    cbnz x5, .Lgx_not_ours
    mov x5, #1
    str x5, [x4, #1016]
    ldp x5, x6, [x1, #0]
    stp x5, x6, [x4, #0]
    ldp x5, x6, [x1, #16]
    stp x5, x6, [x4, #16]
    ldp x5, x6, [x1, #32]
    stp x5, x6, [x4, #32]
    ldp x5, x6, [x1, #48]
    stp x5, x6, [x4, #48]
    ldp x5, x6, [x1, #64]
    stp x5, x6, [x4, #64]
    ldp x5, x6, [x1, #80]
    stp x5, x6, [x4, #80]
    ldp x5, x6, [x1, #96]
    stp x5, x6, [x4, #96]
    ldr x5, [x1, #112]
    str x5, [x4, #112]
    adrp x5, gx_fault_tramp
    add x5, x5, #:lo12:gx_fault_tramp
    str x5, [x1, #88]
    str x4, [x1, #80]
    str x4, [x1, #0]
    mov w0, wzr
    bl svcReturnFromException
    b .
.Lgx_old:
    // gx, a second addition: a jump or an access that memory refused may be
    // to memory that is writable or executable by turns (gmem.c), which the
    // fault is there to turn around. To find out takes C, and everything C
    // may change is the thread's still, so it is kept: x9-x18 and the vector
    // registers.
    ldr w2, [x1, #108]
    lsr w2, w2, #26
    cmp w2, #0x20               // instruction abort
    beq 1f
    cmp w2, #0x24               // data abort
    beq 1f
    // gx, a third: an instruction the processor does not have (class 0) may
    // be one of a newer processor's that gmem.c replaces by what this one
    // can do. The same way round.
    cbnz w2, .Lgx_not_ours
1:  mov x3, sp
    // gx, a fourth: with memory that is committed when first used (gmem.c),
    // what refused the access may be the very stack the thread is on, and
    // the C below, run on that stack, would fault in its turn at the next
    // page down: with the locks it holds by then, which this thread would
    // take a second time and so undo. Each thread of ours has room kept for
    // this at the far end of its own stack (host_nx.c, fault_stack()),
    // found by the thread's local storage, which the guest cannot change.
    mrs x5, tpidrro_el0
    adrp x6, gx_fault_stacks
    add x6, x6, #:lo12:gx_fault_stacks
    mov x7, #128
9:  ldp x8, x4, [x6], #16       // a thread's local storage, and its room
    cmp x8, x5
    beq 8f
    subs x7, x7, #1
    bne 9b
    b 7f
8:  cbnz x4, 6f
7:
    // The stack to use is the one the thread was on (sp of the context),
    // not the one given here: that one is a corner of the thread's local
    // storage, with room for little. Below the context too, should the
    // kernel have put it on that stack.
    ldr x4, [x1, #80]
    sub x5, x4, x1
    cmp x5, #4096
    csel x4, x1, x4, lo
6:  sub x4, x4, #1024
    and x4, x4, #~15
    stp x0, x1, [x4, #0]
    stp x9, x10, [x4, #16]
    stp x11, x12, [x4, #32]
    stp x13, x14, [x4, #48]
    stp x15, x16, [x4, #64]
    stp x17, x18, [x4, #80]
    stp x3, x29, [x4, #96]
    mrs x5, fpcr
    mrs x6, fpsr
    stp x5, x6, [x4, #112]
    add x5, x4, #128
    .irp n, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
    str q\n, [x5, #(\n * 16)]
    .endr
    mov sp, x4
    cbnz w2, 2f
    ldr x0, [x1, #88]           // where the instruction is that the processor does not have
    mov w1, #2
    b 3f
2:  ldr x0, [x1, #112]          // the address that was refused
    cmp w2, #0x20
    cset w1, eq                 // as a jump?
3:  mov x2, xzr                 // nothing kept of the registers to go by
    bl gx_nx_fault
    mov w7, w0
    mov x4, sp
    add x5, x4, #128
    .irp n, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
    ldr q\n, [x5, #(\n * 16)]
    .endr
    ldp x5, x6, [x4, #112]
    msr fpcr, x5
    msr fpsr, x6
    ldp x9, x10, [x4, #16]
    ldp x11, x12, [x4, #32]
    ldp x13, x14, [x4, #48]
    ldp x15, x16, [x4, #64]
    ldp x17, x18, [x4, #80]
    ldp x3, x29, [x4, #96]
    ldp x0, x1, [x4, #0]
    mov sp, x3
    cbz w7, .Lgx_not_ours
    mov w0, wzr                 // turned around: the thread tries again
    bl svcReturnFromException
    b .
.Lgx_not_ours:

    // Abort exception handling when __libnx_exception_handler is not defined.
    adrp x5, :got:__libnx_exception_handler
    ldr x5, [x5, #:got_lo12:__libnx_exception_handler]
    cmp x5, #0
    beq __libnx_exception_entry_abort

    // Load IsCurrentProcessBeingDebugged.

    stp x9, x10, [sp, #-16]!
    stp x11, x12, [sp, #-16]!
    stp x13, x14, [sp, #-16]!
    stp x15, x16, [sp, #-16]!
    stp x17, x18, [sp, #-16]!
    stp x19, x20, [sp, #-16]!
    str x21, [sp, #-16]!

    stp x0, x1, [sp, #-16]!
    sub sp, sp, #16

    mov x0, sp
    mov x1, #8
    mov w2, wzr
    mov x3, #0
    bl svcGetInfo
    mov w6, w0
    ldr x7, [sp], #16

    ldp x0, x1, [sp], #16

    ldr x21, [sp], #16
    ldp x19, x20, [sp], #16
    ldp x17, x18, [sp], #16
    ldp x15, x16, [sp], #16
    ldp x13, x14, [sp], #16
    ldp x11, x12, [sp], #16
    ldp x9, x10, [sp], #16

    // Abort when svcGetInfo failed.
    cbnz w6, __libnx_exception_entry_abort

    // Abort when IsCurrentProcessBeingDebugged is set where __nx_exception_ignoredebug==0.
    adrp x6, __nx_exception_ignoredebug
    ldr  w5, [x6, #:lo12:__nx_exception_ignoredebug]

    cbnz w5, __libnx_exception_entry_start
    cbnz x7, __libnx_exception_entry_abort

__libnx_exception_entry_start:
    adrp x2, __nx_exceptiondump
    add  x2, x2, #:lo12:__nx_exceptiondump
    mov x5, x2

    // error_desc
    str w0, [x2], #4
    // pad
    str wzr, [x2], #4
    str wzr, [x2], #4
    str wzr, [x2], #4

    // GPRs 0..8
    ldp x3, x4, [x1]
    str x5, [x1], #16 // x0 = __nx_exceptiondump
    stp x3, x4, [x2], #16
    ldp x3, x4, [x1], #16
    stp x3, x4, [x2], #16
    ldp x3, x4, [x1], #16
    stp x3, x4, [x2], #16
    ldp x3, x4, [x1], #16
    stp x3, x4, [x2], #16
    ldr x3, [x1], #8
    str x3, [x2], #8

    // GPRs 9..28
    str x9, [x2], #8
    stp x10, x11, [x2], #16
    stp x12, x13, [x2], #16
    stp x14, x15, [x2], #16
    stp x16, x17, [x2], #16
    stp x18, x19, [x2], #16
    stp x20, x21, [x2], #16
    stp x22, x23, [x2], #16
    stp x24, x25, [x2], #16
    stp x26, x27, [x2], #16
    str x28, [x2], #8

    // fp
    str x29, [x2], #8

    // lr
    ldr x3, [x1], #8
    str x3, [x2], #8

    // sp
    adrp x4, __nx_exception_stack
    add  x4, x4, #:lo12:__nx_exception_stack

    adrp x5, __nx_exception_stack_size
    ldr  x5, [x5, #:lo12:__nx_exception_stack_size]
    add x4, x4, x5

    ldr x3, [x1]
    str x4, [x1], #8 // sp = __nx_exception_stack + __nx_exception_stack_size
    str x3, [x2], #8

    // elr_el1 (pc)
    adrp x4, __libnx_exception_returnentry
    add  x4, x4, #:lo12:__libnx_exception_returnentry

    ldr x3, [x1]
    str x4, [x1], #8 // elr_el1 = __libnx_exception_returnentry
    str x3, [x2], #8

    // padding
    str xzr, [x2], #8

    // fpu_gprs
    stp q0, q1, [x2], #32
    stp q2, q3, [x2], #32
    stp q4, q5, [x2], #32
    stp q6, q7, [x2], #32
    stp q8, q9, [x2], #32
    stp q10, q11, [x2], #32
    stp q12, q13, [x2], #32
    stp q14, q15, [x2], #32
    stp q16, q17, [x2], #32
    stp q18, q19, [x2], #32
    stp q20, q21, [x2], #32
    stp q22, q23, [x2], #32
    stp q24, q25, [x2], #32
    stp q26, q27, [x2], #32
    stp q28, q29, [x2], #32
    stp q30, q31, [x2], #32

    // 4 u32s: pstate, afsr0, afsr1, and esr.
    ldr w3, [x1], #4
    str w3, [x2], #4
    ldr w3, [x1], #4
    str w3, [x2], #4
    ldr w3, [x1], #4
    str w3, [x2], #4
    ldr w3, [x1], #4
    str w3, [x2], #4

    //far
    ldr x3, [x1], #8
    str x3, [x2], #8

    mov w0, wzr
    b __libnx_exception_entry_end

__libnx_exception_entry_abort:
    mov w0, #0xf801
__libnx_exception_entry_end:
    bl svcReturnFromException
    b .
CODE_END

// Jumped to by kernel in svcReturnFromException via the overridden elr_el1, with x0 set to __nx_exceptiondump.
CODE_BEGIN __libnx_exception_returnentry
    bl __libnx_exception_handler

    mov w0, wzr
    mov x1, #0
    mov x2, #0
    bl svcBreak
    b .
CODE_END


// gx: do the vector registers of a thread last, once it has come back from
// a fault handled above? x0: 0 to only look, 1 to first read the virtual
// counter (a fault, turned around above), 2 to then make a call to the kernel
// that comes back the usual way; x1: how long to look, in turns. Returns how
// many times a register no longer held what was put in it. The registers
// looked at are of those a function need not keep (v0, v7, v16, v31).
CODE_BEGIN gx_vector_test
    stp x29, x30, [sp, #-48]!
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    mov x19, x0
    mov x20, x1
    mov x21, #0
    movz x22, #0x5a5a
    movk x22, #0xa5a5, lsl #16
    movk x22, #0x1234, lsl #32
    movk x22, #0xfedc, lsl #48
    cbz x19, 1f
    mrs x9, cntvct_el0
    cmp x19, #2
    bne 1f
    svc 0x10                    // which processor this is: asks nothing
1:  fmov d0, x22
    fmov d7, x22
    fmov d16, x22
    fmov d31, x22
2:  fmov x9, d0
    fmov x10, d7
    fmov x11, d16
    fmov x12, d31
    cmp x9, x22
    ccmp x10, x22, #0, eq
    ccmp x11, x22, #0, eq
    ccmp x12, x22, #0, eq
    beq 3f
    add x21, x21, #1
    fmov d0, x22
    fmov d7, x22
    fmov d16, x22
    fmov d31, x22
3:  subs x20, x20, #1
    bne 2b
    mov x0, x21
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x29, x30, [sp], #48
    ret
CODE_END

// gx: and across the fault itself? x0 times over: the registers are filled,
// x1 turns go by without them (time for the processor to be taken away and
// given back, which leaves them to be fetched when next used), the virtual
// counter is read (the fault) and the registers are looked at. Returns how
// many times they no longer held what was put in them.
CODE_BEGIN gx_vector_test_across
    stp x29, x30, [sp, #-48]!
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    mov x19, x0
    mov x20, x1
    mov x21, #0
    movz x22, #0x5a5a
    movk x22, #0xa5a5, lsl #16
    movk x22, #0x1234, lsl #32
    movk x22, #0xfedc, lsl #48
1:  fmov d0, x22
    fmov d7, x22
    fmov d16, x22
    fmov d31, x22
    mov x13, x20
    cbz x13, 3f
2:  subs x13, x13, #1
    bne 2b
3:  mrs x9, cntvct_el0
    fmov x9, d0
    fmov x10, d7
    fmov x11, d16
    fmov x12, d31
    cmp x9, x22
    ccmp x10, x22, #0, eq
    ccmp x11, x22, #0, eq
    ccmp x12, x22, #0, eq
    cinc x21, x21, ne
    subs x19, x19, #1
    bne 1b
    mov x0, x21
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x29, x30, [sp], #48
    ret
CODE_END

// gx: where a fault of the guest's is looked at (see the entry above). x0 and
// sp: what was kept of it, at the far end of this thread's stack, with room
// from 128 on for what is kept here and the stack of the C below it.
// x1-x8 and x9-x30 are the guest's still.
    .global gx_fault_tramp_end
    .global gx_fault_tramp_again
CODE_BEGIN gx_fault_tramp
    str xzr, [x0, #968]         // the way back has not been begun again yet
    stp x9, x10, [x0, #128]
    stp x11, x12, [x0, #144]
    stp x13, x14, [x0, #160]
    stp x15, x16, [x0, #176]
    stp x17, x18, [x0, #192]
    stp x29, x30, [x0, #208]
    mrs x9, fpcr
    mrs x10, fpsr
    stp x9, x10, [x0, #224]
    add x9, x0, #256
    .irp n, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
    str q\n, [x9, #(\n * 16)]
    .endr
    ldr w2, [x0, #108]
    lsr w2, w2, #26
    cbnz w2, 1f
    ldr x0, [sp, #88]           // where the instruction is that the processor does not have
    mov w1, #2
    b 2f
1:  cmp w2, #0x18
    bne 4f
    ldr x0, [sp, #88]           // where the virtual counter is read
    mov w1, #3
    b 2f
4:  ldr x0, [sp, #112]          // the address that was refused
    cmp w2, #0x20
    cset w1, eq                 // as a jump?
2:  mov x2, sp                  // all the guest's registers are there: see gx_nx_fault()
    add x9, x2, #768
    stp x19, x20, [x9, #0]
    stp x21, x22, [x9, #16]
    stp x23, x24, [x9, #32]
    stp x25, x26, [x9, #48]
    stp x27, x28, [x9, #64]
    bl gx_nx_fault
    add x9, sp, #768            // C may have given them new values
    ldp x19, x20, [x9, #0]
    ldp x21, x22, [x9, #16]
    ldp x23, x24, [x9, #32]
    ldp x25, x26, [x9, #48]
    ldp x27, x28, [x9, #64]
    cbnz w0, 3f
    mov x9, #1                  // not ours: it comes again, and is said of
    str x9, [sp, #1008]
3:
gx_fault_tramp_again:           // and from the entry above, when the way back was too long
    mov x0, sp
    str xzr, [x0, #1016]        // nothing from here on can fault
    add x9, x0, #256
    .irp n, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
    ldr q\n, [x9, #(\n * 16)]
    .endr
    ldp x9, x10, [x0, #224]
    msr fpcr, x9
    msr fpsr, x10
    ldp x9, x10, [x0, #128]
    ldp x11, x12, [x0, #144]
    ldp x13, x14, [x0, #160]
    ldp x15, x16, [x0, #176]
    ldp x17, x18, [x0, #192]
    ldp x29, x30, [x0, #208]
gx_fault_tramp_end:
    brk #0x4758
    b .
CODE_END
