/*
 * P0a: one boot answers whether the delivery path works at all.
 *   - ABL boots us from a kernel-only boot.img v4 via `fastboot boot`
 *   - which EL we enter at, and whether the EL2 -> EL1 drop holds
 *   - where ABL put us and the FDT it passed (x0)
 *   - the CPU feature registers XNU cares about (16K granule, PAC)
 *   - PSCI/SMCCC versions and whether a warm reset is on offer
 * Everything goes to the ramoops console zone, then we warm-reset so
 * Android shows it as /sys/fs/pstore/console-ramoops-0.
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"

#define PSCI_VERSION		0x84000000UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define PSCI_FEATURES		0x8400000aUL
#define PSCI_SYSTEM_RESET2	0xc4000012UL
#define PSCI_SYSTEM_WARM_RESET	0
#define SMCCC_VERSION		0x80000000UL

#define FDT_MAGIC		0xd00dfeedU

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define isb() __asm__ volatile("isb" ::: "memory")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }

extern char _head[], _end[];

static const char *stage;	/* set at runtime: no absolute pointers in data */

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

/* Warm reset keeps DRAM, and with it the ramoops zone. */
static void __attribute__((noreturn)) reset(unsigned after_s)
{
	int warm = (int64_t)smc(PSCI_FEATURES, PSCI_SYSTEM_RESET2, 0, 0) >= 0;

	con_str("RESET", warm ? "SYSTEM_RESET2 warm" : "SYSTEM_RESET");
	delay_ms(after_s * 1000);
	if (warm)
		smc(PSCI_SYSTEM_RESET2, PSCI_SYSTEM_WARM_RESET, 0, 0);
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	con_str("RESET", "returned, parking");
	for (;;)
		__asm__ volatile("wfe");
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	switch (slot & 3) {
	case 0: con_str("PANIC", "SYNC"); break;
	case 1: con_str("PANIC", "IRQ"); break;
	case 2: con_str("PANIC", "FIQ"); break;
	default: con_str("PANIC", "SERROR"); break;
	}
	con_hex2("SLOT", "n=", slot, "cpu=", rsr("mpidr_el1"));
	con_str("STAGE", stage);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("SPSR", rsr("spsr_el1"));
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
	con_clear(BLUE);
	con_str("HUSKY", "p0a shim");
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
	con_str("HUSKY", "p0a done");
	reset(2);
}
