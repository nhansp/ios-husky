/*
 * husky boot shim entry: arm64 Linux Image header so the Pixel ABL treats
 * us like a GKI kernel. ABL enters at EL2 (husky is GKI: Linux logs "All
 * CPU(s) started at EL2"), MMU/D-cache off, x0 = DTB pointer. We drop to
 * EL1 straight away so everything after this file runs where XNU will.
 */
	.section .text.head, "ax"
	.globl _head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0			/* text_offset */
	.quad	_end			/* image_size (linked at 0) */
	.quad	0xa			/* flags: LE, 4K pages, place anywhere */
	.quad	0			/* res2 */
	.quad	0			/* res3 */
	.quad	0			/* res4 */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* res5 (PE offset) */

	.text
	.globl _start
_start:
#ifdef PROBE_RAMOOPS
	/*
	 * Decisive handoff probe. Before touching anything (no EL drop, no
	 * SMC, no stack, no cache ops), stamp the ramoops console zone with a
	 * valid persistent_ram_buffer header and the literal bytes "RUN!",
	 * then spin. MMU is off so these stores land straight in DRAM. If the
	 * watchdog resets us and Android later shows "RUN!" in
	 * console-ramoops-0, ABL genuinely transferred control to our entry.
	 * x9 also records CurrentEL as an ASCII digit after "RUN!".
	 */
	movz	x8, #0xf000
	movk	x8, #0xfd3f, lsl #16		/* x8 = 0xfd3ff000 */
	movz	w9, #0x4244
	movk	w9, #0x4347, lsl #16		/* sig "DBGC" */
	str	w9, [x8]
	mov	w9, #6
	str	w9, [x8, #4]			/* start = 6 */
	str	w9, [x8, #8]			/* size  = 6 */
	movz	w9, #0x5552
	movk	w9, #0x214e, lsl #16		/* "RUN!" */
	str	w9, [x8, #12]
	mrs	x9, CurrentEL
	lsr	x9, x9, #2
	add	w9, w9, #0x30			/* '0' + EL */
	strb	w9, [x8, #16]
	mov	w9, #0x0a
	strb	w9, [x8, #17]			/* newline */
	dsb	sy
1:	b	1b
#endif
#ifdef HANG_AT_ENTRY
	/* Bring-up probe: a hung phone means ABL really jumped here. */
1:	b	1b
#endif
	msr	daifset, #0xf
	mov	x19, x0			/* DTB pointer from ABL */
	mrs	x20, CurrentEL		/* entry EL, reported by shim_main */
	/*
	 * Breadcrumb first, before the EL drop can fault: ask EL3 to set the
	 * Pixel reboot mode to NORMAL (SMC_CMD_PRIV_REG write to PMU_ALIVE +
	 * SYSIP_DAT0, as pixel-zuma-reboot.c does). Entered from `fastboot
	 * boot` the mode still reads BOOTLOADER, so if we ever reach here the
	 * next reset lands in Android instead of fastboot and the ramoops log
	 * is readable. x19/x20 are callee-saved across the SMC.
	 */
	movz	w0, #0x0504
	movk	w0, #0x8200, lsl #16	/* SMC_CMD_PRIV_REG */
	movz	x1, #0x0810
	movk	x1, #0x1546, lsl #16	/* PMU_ALIVE_BASE + SYSIP_DAT0 */
	mov	x2, #1			/* PRIV_REG_OPTION_WRITE */
	mov	x3, #0			/* REBOOT_MODE_NORMAL */
	smc	#0
	cmp	x20, #(2 << 2)
	b.ne	1f
	adr	x1, vectors		/* catch faults in the drop itself */
	msr	vbar_el2, x1
	isb
	bl	el2_to_el1
1:	msr	spsel, #1
	adr	x1, _stack_top
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	/* zero .bss */
	adr	x1, _bss_start
	adr	x2, _bss_end
1:	cmp	x1, x2
	b.hs	2f
	str	xzr, [x1], #8
	b	1b
2:	mov	x0, x19
	lsr	x1, x20, #2
	bl	shim_main
3:	wfe
	b	3b

/* PSCI CPU_ON target. x0 = context id = cpu index (1..8). */
	.globl secondary_entry
secondary_entry:
	msr	daifset, #0xf
	mov	x19, x0
	mrs	x1, CurrentEL
	cmp	x1, #(2 << 2)
	b.ne	1f
	bl	el2_to_el1
1:	mov	x0, x19
	msr	spsel, #1
	adr	x1, _sec_stacks
	add	x1, x1, x0, lsl #12
	add	x1, x1, #0x1000
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	bl	secondary_main
4:	wfe
	b	4b

/*
 * Leave EL2 for EL1h and return to the caller there, with DAIF masked.
 * Follows Linux init_kernel_el/init_el2_state for an nVHE host: EL1 gets
 * the timers, the GIC system registers, the PMU counters and the PAC
 * keys/instructions without trapping, and reads the real MIDR/MPIDR.
 * Clobbers x0-x1; x2-x30 survive the eret.
 */
el2_to_el1:
	mov	x0, #(1 << 31)			/* RW: EL1 is AArch64 */
	orr	x0, x0, #(1 << 40)		/* APK: PAC key regs don't trap */
	orr	x0, x0, #(1 << 41)		/* API: PAC instructions don't trap */
	msr	hcr_el2, x0			/* also clears E2H if ABL set it */
	isb
	mov	x0, #0x22ff			/* CPTR_NVHE_EL2_RES1, no FP/SVE trap */
	msr	cptr_el2, x0
	msr	hstr_el2, xzr
	mrs	x0, pmcr_el0
	ubfx	x0, x0, #11, #5			/* HPMN = PMCR_EL0.N */
	msr	mdcr_el2, x0
	mov	x0, #3				/* EL1PCTEN | EL1PCEN */
	msr	cnthctl_el2, x0
	msr	cntvoff_el2, xzr
	mrs	x0, icc_sre_el2
	orr	x0, x0, #(1 << 0)		/* SRE */
	orr	x0, x0, #(1 << 3)		/* Enable: EL1 may set its SRE */
	msr	icc_sre_el2, x0
	isb
	msr	ich_hcr_el2, xzr
	msr	vttbr_el2, xzr
	mrs	x0, midr_el1
	msr	vpidr_el2, x0
	mrs	x0, mpidr_el1
	msr	vmpidr_el2, x0
	/* FEAT_FGT: clear the fine-grained traps ABL may have left. */
	mrs	x0, id_aa64mmfr0_el1
	ubfx	x0, x0, #56, #4
	cbz	x0, 1f
	msr	S3_4_C1_C1_4, xzr		/* HFGRTR_EL2 */
	msr	S3_4_C1_C1_5, xzr		/* HFGWTR_EL2 */
	msr	S3_4_C1_C1_6, xzr		/* HFGITR_EL2 */
	msr	S3_4_C3_C1_4, xzr		/* HDFGRTR_EL2 */
	msr	S3_4_C3_C1_5, xzr		/* HDFGWTR_EL2 */
1:	/* FEAT_HCX */
	mrs	x0, id_aa64mmfr1_el1
	ubfx	x0, x0, #40, #4
	cbz	x0, 2f
	msr	S3_4_C1_C2_2, xzr		/* HCRX_EL2 */
2:	ldr	x0, =0x30d00800			/* SCTLR_EL1 RES1, LE, MMU/caches off */
	msr	sctlr_el1, x0
	mov	x0, #0x3c5			/* EL1h, DAIF masked */
	msr	spsr_el2, x0
	msr	elr_el2, x30
	isb
	eret

/*
 * Exception vectors. Each slot stashes x29/x30, loads its slot number
 * and jumps to the common path, which saves x0-x28 and calls
 * exc_handler(slot, regs). Also installed at VBAR_EL2 while we drop to
 * EL1. IRQs return; everything else is logged to ramoops and resets.
 */
.macro vent slot
	.balign	0x80
	stp	x29, x30, [sp, #-16]!
	mov	x29, #\slot
	b	exc_common
.endm

	.balign	0x800
	.globl vectors
vectors:
	vent 0
	vent 1
	vent 2
	vent 3
	vent 4
	vent 5
	vent 6
	vent 7
	vent 8
	vent 9
	vent 10
	vent 11
	vent 12
	vent 13
	vent 14
	vent 15

exc_common:
	sub	sp, sp, #(30 * 8)
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	str	x28, [sp, #224]
	mov	x0, x29
	mov	x1, sp
	bl	exc_handler
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldr	x28, [sp, #224]
	add	sp, sp, #(30 * 8)
	ldp	x29, x30, [sp], #16
	eret
