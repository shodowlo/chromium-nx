/*
 * tramp.S - guest syscall trampoline.
 *
 * Every `svc #0` of the guest is replaced by a branch to a veneer that pushes
 * x16/x17 and jumps here with x16 = the veneer's exit half. We save the whole
 * guest state in a gx_frame on the guest stack, switch to the host thread
 * pointer and stack of this guest thread, and call gx_do_syscall(frame).
 *
 * The frame is complete (x0-x30, flags, FP state, q0-q31, TPIDR_EL0), so a
 * copy of it is enough to start a thread: see gx_enter_frame.
 */
#define FRAME 800
#define MAX_THREADS 256

	.text
	.global gx_svc_trampoline
	.type gx_svc_trampoline, %function
	.balign 4
gx_svc_trampoline:
	sub	sp, sp, #FRAME
	stp	x0, x1,   [sp, #0]
	stp	x2, x3,   [sp, #16]
	stp	x4, x5,   [sp, #32]
	stp	x6, x7,   [sp, #48]
	stp	x8, x9,   [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x18, x30, [sp, #128]
	mrs	x9, tpidr_el0
	stp	x16, x9,  [sp, #144]
	mrs	x10, nzcv
	mrs	x11, fpcr
	stp	x10, x11, [sp, #160]
	mrs	x10, fpsr
	stp	x10, x19, [sp, #176]
	stp	x20, x21, [sp, #192]
	stp	q0, q1,   [sp, #224]
	stp	q2, q3,   [sp, #256]
	stp	q4, q5,   [sp, #288]
	stp	q6, q7,   [sp, #320]
	stp	q8, q9,   [sp, #352]
	stp	q10, q11, [sp, #384]
	stp	q12, q13, [sp, #416]
	stp	q14, q15, [sp, #448]
	stp	q16, q17, [sp, #480]
	stp	q18, q19, [sp, #512]
	stp	q20, q21, [sp, #544]
	stp	q22, q23, [sp, #576]
	stp	q24, q25, [sp, #608]
	stp	q26, q27, [sp, #640]
	stp	q28, q29, [sp, #672]
	stp	q30, q31, [sp, #704]
	add	x10, sp, #736		/* past the reach of stp from sp */
	stp	x22, x23, [x10, #0]
	stp	x24, x25, [x10, #16]
	stp	x26, x27, [x10, #32]
	stp	x28, x29, [x10, #48]
	mov	x19, sp

	/* Which guest thread is this? Its TPIDR_EL0 (x9) says; if no thread is
	 * known by it, it is one that has just set its own, and where its
	 * stack is says which (gx_thread_at(), which is careful to need
	 * nothing of the host's thread: we are still on the guest's). */
	adrp	x10, gx_tp_tab
	add	x10, x10, :lo12:gx_tp_tab
	mov	x11, #MAX_THREADS
1:	ldp	x12, x13, [x10], #16
	cmp	x12, x9
	b.eq	2f
	subs	x11, x11, #1
	b.ne	1b
3:	mov	x0, x9
	mov	x1, x19
	bl	gx_thread_at
	mov	x13, x0
	b	4f
2:	ldr	x12, [x13, #16]		/* others with the same TPIDR_EL0? */
	cbnz	x12, 3b
4:	str	x13, [x19, #216]
	ldp	x10, x11, [x13]		/* host_tp, host_sp */
	msr	tpidr_el0, x10
	mov	sp, x11
	mov	x0, x19
	bl	gx_do_syscall
	str	x0, [x19, #208]

gx_resume_frame:			/* x19 = frame */
	mov	sp, x19
	ldp	q0, q1,   [sp, #224]
	ldp	q2, q3,   [sp, #256]
	ldp	q4, q5,   [sp, #288]
	ldp	q6, q7,   [sp, #320]
	ldp	q8, q9,   [sp, #352]
	ldp	q10, q11, [sp, #384]
	ldp	q12, q13, [sp, #416]
	ldp	q14, q15, [sp, #448]
	ldp	q16, q17, [sp, #480]
	ldp	q18, q19, [sp, #512]
	ldp	q20, q21, [sp, #544]
	ldp	q22, q23, [sp, #576]
	ldp	q24, q25, [sp, #608]
	ldp	q26, q27, [sp, #640]
	ldp	q28, q29, [sp, #672]
	ldp	q30, q31, [sp, #704]
	add	x9, sp, #736
	ldp	x22, x23, [x9, #0]
	ldp	x24, x25, [x9, #16]
	ldp	x26, x27, [x9, #32]
	ldp	x28, x29, [x9, #48]
	ldp	x16, x9,  [sp, #144]
	msr	tpidr_el0, x9
	ldp	x9, x10,  [sp, #160]
	msr	nzcv, x9
	msr	fpcr, x10
	ldp	x9, x19,  [sp, #176]
	msr	fpsr, x9
	ldp	x20, x21, [sp, #192]
	ldp	x18, x30, [sp, #128]
	ldp	x14, x15, [sp, #112]
	ldp	x12, x13, [sp, #96]
	ldp	x10, x11, [sp, #80]
	ldp	x8, x9,   [sp, #64]
	ldp	x6, x7,   [sp, #48]
	ldp	x4, x5,   [sp, #32]
	ldp	x2, x3,   [sp, #16]
	ldr	x1,       [sp, #8]
	ldr	x0,       [sp, #208]
	add	sp, sp, #FRAME
	br	x16
	.size gx_svc_trampoline, .-gx_svc_trampoline

/*
 * void gx_enter_frame(struct gx_frame *frame, struct gx_thread *self)
 * Leaves the host for the guest state in frame. The host stack and thread
 * pointer of the caller become the ones this guest thread's syscalls run on.
 */
	.global gx_enter_frame
	.type gx_enter_frame, %function
	.balign 4
gx_enter_frame:
	mrs	x9, tpidr_el0
	mov	x10, sp
	stp	x9, x10, [x1]
	mov	x19, x0
	b	gx_resume_frame
	.size gx_enter_frame, .-gx_enter_frame

/*
 * void gx_resume(struct gx_frame *frame)
 * Leaves the host for the guest state in frame, from inside a syscall: what
 * the host stack held of that syscall is dropped, the thread's next one
 * starts where they all do. For signals: into a handler, and back out of it.
 */
	.global gx_resume
	.type gx_resume, %function
	.balign 4
gx_resume:
	mov	x19, x0
	b	gx_resume_frame
	.size gx_resume, .-gx_resume

/*
 * gx_hostcall: calls the host function in x16 for the guest, with the guest's
 * arguments. A guest library reaches the host this way (see guest/hostlib.c):
 * guest and host follow the same calling convention, so all there is to do is
 * to run the function on the host thread pointer and stack of this thread.
 * x0-x8 and v0-v7 pass through, and so do eight words of stack arguments.
 *
 * The host function must not call back into the guest: a syscall made from
 * there would start from the same host stack pointer.
 */
	.global gx_hostcall
	.type gx_hostcall, %function
	.balign 4
gx_hostcall:
	mrs	x9, tpidr_el0
	adrp	x10, gx_tp_tab
	add	x10, x10, :lo12:gx_tp_tab
	mov	x11, #MAX_THREADS
1:	ldp	x12, x13, [x10], #16
	cmp	x12, x9
	b.eq	2f
	subs	x11, x11, #1
	b.ne	1b
	adrp	x13, gx_main_thread
	ldr	x13, [x13, :lo12:gx_main_thread]
2:	ldp	x10, x11, [x13]		/* host_tp, host_sp */
	mov	x12, sp
	sub	x11, x11, #96
	ldp	x14, x15, [x12, #0]
	stp	x14, x15, [x11, #0]
	ldp	x14, x15, [x12, #16]
	stp	x14, x15, [x11, #16]
	ldp	x14, x15, [x12, #32]
	stp	x14, x15, [x11, #32]
	ldp	x14, x15, [x12, #48]
	stp	x14, x15, [x11, #48]
	stp	x12, x9, [x11, #64]	/* guest sp, guest TPIDR_EL0 */
	str	x30, [x11, #80]
	mov	sp, x11
	msr	tpidr_el0, x10
	blr	x16
	ldp	x12, x9, [sp, #64]
	ldr	x30, [sp, #80]
	msr	tpidr_el0, x9
	mov	sp, x12
	ret
	.size gx_hostcall, .-gx_hostcall

	.section .note.GNU-stack,"",%progbits
