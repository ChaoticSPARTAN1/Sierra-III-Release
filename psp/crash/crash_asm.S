/* halo_crash.prx's exception entry (build 102).

Why not pspsdk's: its stub (_pspDebugExceptionHandler) saves the registers
and then "returns" from the exception into the handler, which runs in the
mode the crashed thread was in. pspsdk was written for firmware where games
ran in kernel mode; ours runs in user mode, and the handler's code is in this
kernel module, so the return lands a user-mode thread on kernel code: another
exception at once, the same handler again, for ever. Every crash in the
hardware runs froze with nothing on the screen, then the PSP switched itself off; so
did the deliberate one (crashplay, b100 run 2).

So, as ARK-4's own crash screen module does (extras/modules/bsod): the
registers go to a block in the game's memory, and the return from the
exception lands in a handler in the game itself (psp_crash.c), user code on a
stack of its own, with a0 pointing at the block. That handler can write
crash.txt and draw the report with ordinary calls.

Two signs on the screen from here, with nothing but stores (no calls are
safe inside the exception): a magenta band across the top 8 lines of both of
the GE's frame buffers when the stub runs, and, if a second exception comes
(the game's handler itself crashed), a yellow band, and it stops there. */

	.set noreorder
	.set noat

#define Status   $12
#define BadVAddr $8
#define Cause    $13
#define EPC      $14

	.data
	.global halo_entries
	.global halo_block
	.global halo_user_handler
	.global halo_user_stack
halo_entries:      .word 0
halo_block:        .word 0
halo_user_handler: .word 0
halo_user_stack:   .word 0

	.text
	.global halo_exception
	.ent halo_exception
halo_exception:
	nop
	nop
	/* v0 and v1 are free here: the exception manager keeps their values
	in COP0's control registers 4 and 5 */
	la		$v0, halo_entries
	lw		$v1, 0($v0)
	addiu	$v1, $v1, 1
	sw		$v1, 0($v0)
	addiu	$v1, $v1, -1
	bnez	$v1, halo_again
	nop
	la		$v0, halo_block
	lw		$v0, 0($v0)
	beqz	$v0, halo_again
	nop
	sw		$0, 0($v0)
	sw		$1, 4($v0)
	cfc0	$1, $4
	sw		$1, 8($v0)
	cfc0	$1, $5
	sw		$1, 12($v0)
	sw		$4, 16($v0)
	sw		$5, 20($v0)
	sw		$6, 24($v0)
	sw		$7, 28($v0)
	sw		$8, 32($v0)
	sw		$9, 36($v0)
	sw		$10, 40($v0)
	sw		$11, 44($v0)
	sw		$12, 48($v0)
	sw		$13, 52($v0)
	sw		$14, 56($v0)
	sw		$15, 60($v0)
	sw		$16, 64($v0)
	sw		$17, 68($v0)
	sw		$18, 72($v0)
	sw		$19, 76($v0)
	sw		$20, 80($v0)
	sw		$21, 84($v0)
	sw		$22, 88($v0)
	sw		$23, 92($v0)
	sw		$24, 96($v0)
	sw		$25, 100($v0)
	sw		$26, 104($v0)
	sw		$27, 108($v0)
	sw		$28, 112($v0)
	sw		$29, 116($v0)
	sw		$30, 120($v0)
	sw		$31, 124($v0)
	mfc0	$v1, Status
	sw		$v1, 128($v0)
	mflo	$v1
	sw		$v1, 132($v0)
	mfhi	$v1
	sw		$v1, 136($v0)
	mfc0	$v1, BadVAddr
	sw		$v1, 140($v0)
	mfc0	$v1, Cause
	sw		$v1, 144($v0)
	mfc0	$v1, EPC
	sw		$v1, 148($v0)

	/* the magenta band: 8 lines of 512 16-bit pixels, both frame buffers */
	lui		$t0, 0x4400
	lui		$t1, 0x4404
	ori		$t1, $t1, 0x4000
	li		$t2, 4096
	li		$t3, 0xF81F
1:
	sh		$t3, 0($t0)
	sh		$t3, 0($t1)
	addiu	$t0, $t0, 2
	addiu	$t1, $t1, 2
	addiu	$t2, $t2, -1
	bnez	$t2, 1b
	nop

	/* out of the exception into the game's handler, on its own stack */
	move	$a0, $v0
	la		$v1, halo_user_stack
	lw		$sp, 0($v1)
	la		$v1, halo_user_handler
	lw		$v1, 0($v1)
	mtc0	$v1, EPC
	nop
	nop
	eret
	nop
	nop

halo_again:
	/* a second exception (or no handler to go to): a yellow band, and stop */
	lui		$t0, 0x4400
	lui		$t1, 0x4404
	ori		$t1, $t1, 0x4000
	li		$t2, 4096
	li		$t3, 0x07FF
2:
	sh		$t3, 0($t0)
	sh		$t3, 0($t1)
	addiu	$t0, $t0, 2
	addiu	$t1, $t1, 2
	addiu	$t2, $t2, -1
	bnez	$t2, 2b
	nop
3:
	b		3b
	nop
	.end halo_exception
