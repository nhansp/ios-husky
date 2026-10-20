/*
 * P0c PAC spike helpers. Isolated in their own unit so the armv8.3-a
 * `.arch` (needed to assemble pac/aut/xpac insns) doesn't leak into the rest
 * of the shim, which is built -mgeneral-regs-only for a plain v8.0 core.
 *
 * iOS arm64e signs pointers with the architected QARMA5 PAC; Apple's
 * kernel owns the keys through Apple-private SPRs + GXF. On husky those
 * SPRs don't exist, so XNU must drive the architected AP*Key_EL1 regs
 * instead. This proves the architected path works at EL1 after our
 * EL2->EL1 drop: keys settable, instructions not trapped to EL3, and a
 * sign/authenticate round-trip that recovers the original pointer.
 */
	.arch	armv8.3-a
	.text

/* uint64_t pac_enable(void) — turn on pac/aut at EL1, return new SCTLR_EL1. */
	.globl	pac_enable
pac_enable:
	mrs	x0, sctlr_el1
	orr	x0, x0, #(1 << 31)	/* EnIA */
	orr	x0, x0, #(1 << 30)	/* EnIB */
	orr	x0, x0, #(1 << 27)	/* EnDA */
	orr	x0, x0, #(1 << 13)	/* EnDB */
	msr	sctlr_el1, x0
	isb
	ret

/* void pac_set_iakey(uint64_t lo, uint64_t hi) — architected APIAKey_EL1. */
	.globl	pac_set_iakey
pac_set_iakey:
	msr	apiakeylo_el1, x0
	msr	apiakeyhi_el1, x1
	isb
	ret

/* void pac_set_gakey(uint64_t lo, uint64_t hi) — architected APGAKey_EL1. */
	.globl	pac_set_gakey
pac_set_gakey:
	msr	apgakeylo_el1, x0
	msr	apgakeyhi_el1, x1
	isb
	ret

/* uint64_t pac_sign_ia(uint64_t ptr, uint64_t mod) — pacia. */
	.globl	pac_sign_ia
pac_sign_ia:
	pacia	x0, x1
	ret

/* uint64_t pac_auth_ia(uint64_t signed_ptr, uint64_t mod) — autia (may FPAC-fault). */
	.globl	pac_auth_ia
pac_auth_ia:
	autia	x0, x1
	ret

/* uint64_t pac_strip_ia(uint64_t signed_ptr) — xpaci (never faults). */
	.globl	pac_strip_ia
pac_strip_ia:
	xpaci	x0
	ret

/* uint64_t pac_gen(uint64_t data, uint64_t mod) — pacga generic MAC in high 32b. */
	.globl	pac_gen
pac_gen:
	pacga	x0, x0, x1
	ret
