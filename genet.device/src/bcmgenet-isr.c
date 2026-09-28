// SPDX-License-Identifier: GPL-2.0+
/*
 * bcmgenet-isr.c - the Exec interrupt server for INTRL2_0, in file-scope assembly.
 *
 * Its own translation unit because file-scope asm() carries no IR: LTO does not know
 * which symbols it defines, and which partition it would land in is unspecified, so this
 * TU is compiled -fno-lto (see the CMakeLists).  Keeping it separate means only these
 * lines opt out rather than bcmgenet-irq.c's configuration helpers with them.
 *
 * The asm cannot move to a .S file: its struct offsets come from offsetof() through "i"
 * operands, which needs the C compiler.
 */
#include <stddef.h> /* offsetof, for the assembly server's operands */
#include <genet/bcmgenet-irq.h>
#include <genet/bcmgenet-regs.h>
#include <device.h>

/* For the assembly interrupt server below: a constant byte-swapped for a
 * little-endian MMIO register.  Folds at compile time. */
#define ISR_SWAP32(v) ((u32)((((u32)(v) & 0x000000ffu) << 24) | \
							 (((u32)(v) & 0x0000ff00u) << 8) |  \
							 (((u32)(v) & 0x00ff0000u) >> 8) |  \
							 (((u32)(v) & 0xff000000u) >> 24)))
/* The same swap applied to a bit NUMBER, for BTST. */
#define ISR_SWAP_BIT(n) (((n) & 7) | ((3 - ((n) >> 3)) << 3))
/* exec.library Signal(), from <exec/exec_lvo.i>; verified against the compiled
 * C version this replaced. */
#define LVO_SIGNAL (-324)


/*
 * bcmgenet_isr0 - the interrupt server gic400 calls for INTRL2_0.
 *
 * In assembly, because the Exec server ABI is a CONDITION CODE, not a return
 * value: exec.library/AddIntServer walks a chain until a server returns with Z
 * clear ("mine, stop here"), and the caller tests the flag, not D0.  Compiled
 * from C this function ended `moveq #1,%d0` / `move.l (%sp)+,%d2` / `rts`, and
 * that MOVE sets Z from the RESTORED register instead of from the return value
 * — exactly the hazard the autodoc warns about.  Whether GCC emits the
 * CCR-transparent MOVEM or a plain MOVE depends on how many registers it chose
 * to save, so it is not something the C source can state or keep.
 *
 * Written out, the body needs no callee-saved register at all: Signal() is the
 * last thing it does, so nothing has to survive a call.  That leaves a bare
 * `rts` for an epilogue and the flag from `moveq` reaching the caller intact.
 *
 * Behaviour is the C version's, minus its interrupt-level KprintfT — the same
 * status word is left in unit->irq0_status for the bottom half to log.
 *
 * Registers: A1 = unit on entry (Exec is_Data), A6 = SysBase, D0 = IRQ number.
 * A0/D0/D1 are scratch here; A1 is reused for Signal()'s task argument once
 * the unit pointer is finished with.
 *
 * The status stays little-endian until the end — a byte swap is a permutation
 * of bits, so it commutes with AND/NOT and leaves Z alone, and ISR_SWAP32 /
 * ISR_SWAP_BIT match the constants to it.  The one NOP (Emu68: dsb sy) is what
 * makes the CPU_CLEAR ack land before the GIC re-samples the line; ordering
 * between the accesses is already guaranteed by Device-nGnRE.
 */
#ifndef __INTELLISENSE__ /* no file-scope extended asm there */
__asm__(
	/* Its own section, as EMU68_INTSERVER() gives a C server one -- see
	 * <intserver.h>.  GCC emits a top-level asm() before any function body and
	 * tracks the current section itself, so this block must hand .text back at the
	 * end or the whole TU follows it in here.  (.pushsection/.popsection would say
	 * it better; this assembler has neither.) */
	"	.section .text.isr.bcmgenet_isr0,\"ax\"\n"
	"	.even\n"
	"	.globl	_bcmgenet_isr0\n"
	"_bcmgenet_isr0:\n"
	"	movea.l	%c[genetbase](%%a1),%%a0\n"   /* A0 = unit->genetBase; the INTRL2_0 */
	"	move.l	%c[stat](%%a0),%%d0\n"        /* offsets are folded into the constants */
	"	move.l	%c[maskstat](%%a0),%%d1\n"    /* D0 = CPU_STAT, D1 = CPU_MASK_STATUS */
	"	not.l	%%d1\n"                       /* status = STAT & ~MASK_STATUS */
	"	and.l	%%d1,%%d0\n"
	"	bne.s	1f\n"
	"	moveq	#0,%%d0\n"                    /* nothing of ours: Z set, chain goes on */
	"	rts\n"                                /* no registers saved, so nothing follows */
	"1:	move.l	%%d0,%c[clear](%%a0)\n"       /* CPU_CLEAR: ack before handling, so an */
	"	addq.l	#1,%c[count](%%a1)\n"         /* event after this point re-asserts */
	"	move.l	%%d0,%%d1\n"
	"	andi.l	#%c[txrx],%%d1\n"
	"	beq.s	5f\n"                         /* neither: link/PHY/error, kept out of line */
	"	move.l	%%d1,%c[maskset](%%a0)\n"     /* MASK_SET is write-1-to-set, so this one */
	"	btst	#%c[txbit],%%d0\n"            /* write masks exactly the ones that fired, */
	"	beq.s	2f\n"                         /* until the task catches up */
	"	addq.l	#1,%c[txcount](%%a1)\n"
	"2:	btst	#%c[rxbit],%%d0\n"
	"	beq.s	3f\n"
	"	addq.l	#1,%c[rxcount](%%a1)\n"
	"3:	ror.w	#8,%%d0\n"                    /* the one byte swap: irq0_status is read */
	"	swap	%%d0\n"                       /* with native-order masks */
	"	ror.w	#8,%%d0\n"
	"	or.l	%%d0,%c[status](%%a1)\n"      /* hand the status to the bottom half */
	"	move.b	%c[signal](%%a1),%%d1\n"
	"	extb.l	%%d1\n"
	"	moveq	#1,%%d0\n"
	"	lsl.l	%%d1,%%d0\n"                  /* D0 = 1 << unit->irq0_signal */
	"	movea.l	%c[task](%%a1),%%a1\n"        /* A1 = unit->task; the unit is finished with */
	"	jsr	%c[lvosignal](%%a6)\n"            /* Signal(task, mask) */
	"	nop\n"                                /* dsb sy: the ack must land before we return */
	"	moveq	#1,%%d0\n"                    /* handled: Z clear ends the chain walk */
	"	rts\n"
	"5:	addq.l	#1,%c[othercount](%%a1)\n"    /* cold, so out of the TX/RX fall-through */
	"	bra.s	3b\n"
	"	.text\n"                              /* back to where GCC thinks it is */
	:
	: [genetbase] "i"(offsetof(struct GenetUnit, genetBase)),
	  [status] "i"(offsetof(struct GenetUnit, irq0_status)),
	  [signal] "i"(offsetof(struct GenetUnit, irq0_signal)),
	  [task] "i"(offsetof(struct GenetUnit, task)),
	  [count] "i"(offsetof(struct GenetUnit, internalStats.irq0_count)),
	  [txcount] "i"(offsetof(struct GenetUnit, internalStats.irq0_tx_count)),
	  [rxcount] "i"(offsetof(struct GenetUnit, internalStats.irq0_rx_count)),
	  [othercount] "i"(offsetof(struct GenetUnit, internalStats.irq0_other_count)),
	  [stat] "i"(GENET_INTRL2_0_OFF + INTRL2_CPU_STAT),
	  [maskstat] "i"(GENET_INTRL2_0_OFF + INTRL2_CPU_MASK_STATUS),
	  [clear] "i"(GENET_INTRL2_0_OFF + INTRL2_CPU_CLEAR),
	  [maskset] "i"(GENET_INTRL2_0_OFF + INTRL2_CPU_MASK_SET),
	  [txrx] "i"(ISR_SWAP32(UMAC_IRQ_TXDMA_DONE | UMAC_IRQ_RXDMA_DONE)),
	  [txbit] "i"(ISR_SWAP_BIT(__builtin_ctz(UMAC_IRQ_TXDMA_DONE))),
	  [rxbit] "i"(ISR_SWAP_BIT(__builtin_ctz(UMAC_IRQ_RXDMA_DONE))),
	  [lvosignal] "i"(LVO_SIGNAL));
#endif
