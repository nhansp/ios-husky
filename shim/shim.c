/*
 * P0a: one boot answers whether the delivery path works at all.
 *   - ABL boots us from a kernel-only boot.img v4 via `fastboot boot`
 *   - which EL we enter at, and whether the EL2 -> EL1 drop holds
 *   - where ABL put us and the FDT it passed (x0)
 *   - the CPU feature registers XNU cares about (16K granule, PAC)
 *   - PSCI/SMCCC versions and whether a warm reset is on offer
 * Everything goes to the ramoops console zone, then we reset the way Linux
 * does (PSCI SYSTEM_RESET) so Android shows it as
 * /sys/fs/pstore/console-ramoops-0.
 *
 * Breadcrumb that doesn't depend on DRAM surviving: the first thing we do
 * is set the Pixel reboot mode (PMU_ALIVE SYSIP_DAT0, written through EL3
 * like pixel-zuma-reboot.c does) to NORMAL. Entered from `fastboot boot`
 * the mode still says BOOTLOADER, so landing in Android rather than
 * fastboot means the shim ran.
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"

#define PSCI_VERSION		0x84000000UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define SMCCC_VERSION		0x80000000UL

/* soc/gs exynos-smc.h, exynos-el3_mon.h, pixel-zuma-reboot.c */
#define SMC_CMD_PRIV_REG	0x82000504UL
#define PRIV_REG_OPTION_WRITE	1
#define PMU_ALIVE_BASE		0x15460000UL	/* system-controller@15460000 */
#define PMU_SYSIP_DAT0		0x0810		/* pixel-reboot reboot-cmd-offset */
#define REBOOT_MODE_NORMAL	0x00

#define FDT_MAGIC		0xd00dfeedU

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define isb() __asm__ volatile("isb" ::: "memory")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }

extern char _head[], _end[];

static const char *stage;	/* set at runtime: no absolute pointers in data */

/* P0c PAC spike (pac.S). */
uint64_t pac_enable(void);
void pac_set_iakey(uint64_t lo, uint64_t hi);
void pac_set_gakey(uint64_t lo, uint64_t hi);
uint64_t pac_sign_ia(uint64_t ptr, uint64_t mod);
uint64_t pac_auth_ia(uint64_t signed_ptr, uint64_t mod);
uint64_t pac_strip_ia(uint64_t signed_ptr);
uint64_t pac_gen(uint64_t data, uint64_t mod);

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000 * ms;
	while (now() < end)
		;
}

/* What Linux ends in on a plain reboot; DRAM (and ramoops) survives it. */
static void __attribute__((noreturn)) reset(unsigned after_s)
{
	con_str("RESET", "SYSTEM_RESET");
	delay_ms(after_s * 1000);
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	con_str("RESET", "returned, parking");
	for (;;)
		__asm__ volatile("wfe");
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	int el2 = (rsr("CurrentEL") >> 2) == 2;

	if (el2)	/* faulted in the drop, before shim_main started the log */
		con_clear(BLUE);
	switch (slot & 3) {
	case 0: con_str("PANIC", "SYNC"); break;
	case 1: con_str("PANIC", "IRQ"); break;
	case 2: con_str("PANIC", "FIQ"); break;
	default: con_str("PANIC", "SERROR"); break;
	}
	con_hex2("SLOT", "n=", slot, "cpu=", rsr("mpidr_el1"));
	con_str("STAGE", stage);
	con_str("EL", el2 ? "2" : "1");
	con_hex("ESR", el2 ? rsr("esr_el2") : rsr("esr_el1"));
	con_hex("ELR", el2 ? rsr("elr_el2") : rsr("elr_el1"));
	con_hex("FAR", el2 ? rsr("far_el2") : rsr("far_el1"));
	con_hex("SPSR", el2 ? rsr("spsr_el2") : rsr("spsr_el1"));
	con_hex("X0", regs[0]);
	con_hex("X1", regs[1]);
	con_hex("X30", regs[29]);	/* stashed lr is just above the frame */
	reset(1);
}

void secondary_main(uint64_t idx)
{
	(void)idx;
	for (;;)
		__asm__ volatile("wfe");
}

static void report_fdt(uintptr_t dtb)
{
	uint64_t v;

	stage = "fdt";
	con_hex("DTB", dtb);
	if (!dtb || (dtb & 3)) {
		con_str("FDT", "no usable pointer");
		return;
	}
	uint32_t magic = __builtin_bswap32(rd32(dtb));
	con_hex2("FDT", "magic=", magic, "size=", __builtin_bswap32(rd32(dtb + 4)));
	if (magic != FDT_MAGIC)
		return;
	if (fdt_chosen_u64(dtb, "linux,initrd-start", &v))
		con_hex("INITRD0", v);
	if (fdt_chosen_u64(dtb, "linux,initrd-end", &v))
		con_hex("INITRD1", v);
}

void shim_main(uint64_t dtb, uint64_t entry_el)
{
	uint64_t mode_ret = smc(SMC_CMD_PRIV_REG, PMU_ALIVE_BASE + PMU_SYSIP_DAT0,
				PRIV_REG_OPTION_WRITE, REBOOT_MODE_NORMAL);

	con_clear(BLUE);
	con_str("HUSKY", "p0a shim");
	con_hex("BOOTMODE", mode_ret);	/* set_priv_reg result, 0 = ok */
	stage = "regs";
	con_hex2("EL", "entry=", entry_el, "now=", rsr("CurrentEL") >> 2);
	con_hex("IMAGE", (uintptr_t)_head);
	con_hex("END", (uintptr_t)_end);
	report_fdt(dtb);

	stage = "cpu";
	con_hex("MIDR", rsr("midr_el1"));
	con_hex("MPIDR", rsr("mpidr_el1"));
	con_hex("ISAR1", rsr("id_aa64isar1_el1"));	/* APA/API/GPA/GPI: PAC */
	con_hex("ISAR2", rsr("S3_0_C0_C6_2"));		/* id_aa64isar2_el1: APA3/GPA3 */
	con_hex("MMFR0", rsr("id_aa64mmfr0_el1"));	/* TGran16, PARange */
	con_hex("MMFR1", rsr("id_aa64mmfr1_el1"));
	con_hex("PFR0", rsr("id_aa64pfr0_el1"));
	con_hex("SCTLR", rsr("sctlr_el1"));
	con_hex("CNTFRQ", rsr("cntfrq_el0"));

	stage = "psci";
	con_hex("PSCI", smc(PSCI_VERSION, 0, 0, 0));
	con_hex("SMCCC", smc(SMCCC_VERSION, 0, 0, 0));

	/*
	 * P0c: PAC at EL1. Set the architected APIAKey/APGAKey, enable the
	 * instructions, and round-trip a canonical TTBR0 pointer. A nonzero
	 * PAC in the signed value proves EL3 is not trapping pac/aut (if it
	 * were, we'd have reset here or seen the input unchanged). recovered
	 * == ptr proves the QARMA5 round-trip. The final corrupt autia should
	 * FPAC-fault (ISAR1.APA=5), logged as a SYNC panic at stage "pacbad".
	 */
	stage = "pac";
	uint64_t ptr = 0x0000000012345670ULL, mod = 0x1122334455667788ULL;
	con_hex("SCTLRpac", pac_enable());
	pac_set_iakey(0x0123456789abcdefULL, 0xfedcba9876543210ULL);
	pac_set_gakey(0x0f1e2d3c4b5a6978ULL, 0x8796a5b4c3d2e1f0ULL);
	uint64_t s1 = pac_sign_ia(ptr, mod);
	uint64_t s2 = pac_sign_ia(ptr, mod);
	uint64_t rec = pac_auth_ia(s1, mod);
	con_hex("PTR", ptr);
	con_hex("PACIA", s1);
	con_hex2("DET", "same=", s1 == s2, "pac=", (uint32_t)((s1 ^ ptr) >> 32));
	con_hex2("RT", "rec=", (uint32_t)rec, "ok=", rec == ptr);
	con_hex("XPACI", pac_strip_ia(s1));
	con_hex("PACGA", pac_gen(ptr, mod));
	con_str("HUSKY", "p0a done");

	stage = "pacbad";
	con_hex("AUTBAD", pac_auth_ia(s1 ^ (1ULL << 50), mod));	/* expect FPAC fault */
	con_str("PAC", "no FPAC fault");

	reset(2);
}
