/*
 * P1.2: act as iBoot for xnu on husky.
 *
 * mkpayload.py resolved the kernel's plain chained rebases and built the ADT,
 * so this: finds the payload in the ABL ramdisk, copies the segments to
 * phys_base + (vmaddr - virt_base), signs the arm64e auth pointers in place
 * with the architected PAC keys (the part mkpayload could not do offline),
 * places the ADT and boot_args behind the kernel, and jumps to the entry
 * point with the MMU and caches off, x0 = boot_args (physical).
 *
 * Unlike sweet (plain arm64, where the kernel's autia are NOPs so unsigned
 * fixup targets just work), husky has real PAC: every auth pointer must carry
 * a valid signature or the kernel faults on first use. We sign at runtime with
 * the CPU, using the fixed keys patch 0002 installs, which the kernel re-installs
 * at PAC_INIT so our signatures authenticate.
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "loader.h"
#include "ufs.h"
#include "fb.h"

#define BOOT_LINE_LENGTH	1024
#define PAGE_16K		0x4000UL

/* The single architected PAC key constant patch 0002 writes to every AP*Key
 * register (lo and hi), i.e. movz 0x4331 / movk 0x5041,0x534b,0x4855. */
#define HUSKY_PAC_KEY		0x4855534b50414331ULL

/* End of the shim image; the payload is appended here (see loader_boot). */
extern char _end[];

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	uint64_t auth_off, auth_count;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* One arm64e auth fixup: slot_va is the kernel VA where the (already
 * unsigned-target) pointer lives; info packs the signing parameters. */
struct auth_fixup {
	uint64_t slot_va;
	uint32_t info;		/* diversity[0:15] | addrDiv<<16 | key<<17 */
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2, BOOT_LINE_LENGTH 1024 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x458, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, bootFlags) == 1136, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 1152, "boot_args layout");

/* MMU off: everything is Device memory, so copy in aligned 8-byte words. */
static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

static int magic_ok(const char *m)
{
	static const char want[] = "HUSKYXNU";
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

/*
 * Fill an 8-byte ADT property placeholder by name (mkadt emits it zeroed).
 * ADT properties are {char name[32]; u32 length; value}, 4-byte aligned, so
 * names sit on 4-byte boundaries. 32-bit stores: Device memory (MMU off)
 * faults on the unaligned 8-byte access the value field may need.
 */
static int adt_set_u64(uintptr_t adt, uint64_t size, const char *name, uint64_t val)
{
	for (uint64_t off = 0; off + 44 <= size; off += 4) {
		volatile const char *n = (volatile const char *)(adt + off);
		int i = 0;
		while (i < 32 && name[i] && n[i] == name[i])
			i++;
		if (i == 32 || name[i] != 0)
			continue;
		while (i < 32 && n[i] == 0)
			i++;
		if (i != 32)
			continue;
		volatile uint32_t *w = (volatile uint32_t *)(adt + off + 32);
		if ((w[0] & 0x7fffffffU) != 8)
			continue;
		w[1] = (uint32_t)val;
		w[2] = (uint32_t)(val >> 32);
		return 1;
	}
	return 0;
}

/*
 * PAC. Install the fixed architected keys (0002's constant) in every key
 * register and enable the pac/aut instructions, so the sign_slot() helpers
 * below produce signatures the kernel will authenticate after PAC_INIT.
 */
static void pac_install_keys(void)
{
	uint64_t k = HUSKY_PAC_KEY;
	__asm__ volatile(
		"msr S3_0_c2_c1_0, %0\n msr S3_0_c2_c1_1, %0\n"	/* APIAKey lo/hi */
		"msr S3_0_c2_c1_2, %0\n msr S3_0_c2_c1_3, %0\n"	/* APIBKey lo/hi */
		"msr S3_0_c2_c2_0, %0\n msr S3_0_c2_c2_1, %0\n"	/* APDAKey lo/hi */
		"msr S3_0_c2_c2_2, %0\n msr S3_0_c2_c2_3, %0\n"	/* APDBKey lo/hi */
		"msr S3_0_c2_c3_0, %0\n msr S3_0_c2_c3_1, %0\n"	/* APGAKey lo/hi */
		"isb\n" :: "r"(k) : "memory");
	uint64_t sctlr;
	__asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
	sctlr |= (1ULL << 13) | (1ULL << 27) | (1ULL << 30) | (1ULL << 31);	/* EnDA,EnDB,EnIB,EnIA */
	__asm__ volatile("msr sctlr_el1, %0\n isb" :: "r"(sctlr) : "memory");
}

/*
 * blend(addr, disc): the arm64e blended discriminator — the 16-bit constant in
 * the top 16 bits, the storage VA in the low 48. Must match clang's
 * __builtin_ptrauth_blend_discriminator (which the kernel's rebase uses), so
 * the kernel authenticates with the same modifier. [Verify on first boot.]
 */
static inline uint64_t blend(uint64_t addr, uint16_t disc)
{
	return (addr & 0x0000ffffffffffffULL) | ((uint64_t)disc << 48);
}

/* Sign ptr with the given key (0=IA 1=IB 2=DA 3=DB) and 64-bit modifier. */
static uint64_t pac_sign(uint64_t ptr, unsigned key, uint64_t mod)
{
	register uint64_t p __asm__("x0") = ptr;
	register uint64_t m __asm__("x1") = mod;
	switch (key) {
	case 0: __asm__ volatile("pacia %0, %1" : "+r"(p) : "r"(m)); break;
	case 1: __asm__ volatile("pacib %0, %1" : "+r"(p) : "r"(m)); break;
	case 2: __asm__ volatile("pacda %0, %1" : "+r"(p) : "r"(m)); break;
	case 3: __asm__ volatile("pacdb %0, %1" : "+r"(p) : "r"(m)); break;
	}
	return p;
}

/* Sign every arm64e auth pointer in place, following rebase_chain()'s rule:
 * discriminator = addrDiv ? (diversity ? blend(slot_va, diversity) : slot_va)
 * : diversity. The slot holds its unsigned target (mkpayload wrote it). */
static void sign_auth_fixups(const struct payload *p, uint64_t pa_of_va_base)
{
	const struct auth_fixup *t = (const struct auth_fixup *)((uintptr_t)p + p->auth_off);
	for (uint64_t i = 0; i < p->auth_count; i++) {
		uint64_t slot_va = t[i].slot_va;
		uint32_t info = t[i].info;
		uint16_t diversity = info & 0xffff;
		unsigned addr_div = (info >> 16) & 1;
		unsigned key = (info >> 17) & 3;
		uint64_t mod = addr_div ? (diversity ? blend(slot_va, diversity) : slot_va) : diversity;
		volatile uint64_t *slot = (volatile uint64_t *)(uintptr_t)(slot_va + pa_of_va_base);
		*slot = pac_sign(*slot, key, mod);
	}
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

const char *loader_boot(uintptr_t dtb)
{
	uint64_t rd_start, rd_end;

	/*
	 * Delivery: `fastboot boot` drops our boot.img ramdisk (ABL uses only the
	 * on-device vendor_boot ramdisk), so the payload is appended to the shim
	 * image itself (after _end, with the Image header's image_size patched so
	 * ABL loads it all). It therefore sits at _end at runtime. Fall back to
	 * scanning the ABL initrd in case it was delivered as a ramdisk instead.
	 */
	const struct payload *p = 0;
	if (magic_ok(_end)) {
		p = (const struct payload *)_end;
	} else if (fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) &&
		   fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end)) {
		con_hex2("INITRD", "lo=", rd_start, "hi=", rd_end);
		for (uintptr_t a = rd_start; a + sizeof(struct payload) <= rd_end; a += 0x1000) {
			if (magic_ok((const char *)a)) {
				p = (const struct payload *)a;
				break;
			}
		}
	}
	if (!p)
		return "no HUSKYXNU payload (not at _end or in initrd)";
	con_hex("PAYLOAD", (uintptr_t)p);
	if (p->version != 2 || (p->adt_off & 7) || (p->auth_off & 7) || ((uintptr_t)p & 7))
		return "bad payload header";

	uint64_t pa_of_va_base = p->phys_base - p->virt_base;	/* pa = va + this */
	con_hex2("PHYS", "base=", p->phys_base, "size=", p->mem_size);
	con_hex2("VIRT", "base=", p->virt_base, "entry=", p->entry_va);
	con_hex2("FIX", "auth=", p->auth_count, "nseg=", p->nsegs);

	/* The payload must not overlap where the kernel goes. */
	if (rd_end > p->phys_base && rd_start < p->phys_base + p->mem_size)
		return "ramdisk overlaps kernel memory";

	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va_base;
		copy64(dst, (uintptr_t)p + s->file_off, s->file_size);
		if (s->vmsize > s->file_size)
			zero64(dst + ((s->file_size + 7) & ~7UL), s->vmsize - ((s->file_size + 7) & ~7UL));
	}
	con_hex2("SEGS", "n=", p->nsegs, "end=", p->kernel_end + pa_of_va_base);

	/* Sign the arm64e auth pointers now that the segments are in place.
	 * HUSKY_NO_PAC_SIGN: leave them as the plain targets mkpayload wrote, for
	 * bring-up against a kernel built with PAC enforcement off (0010). */
#ifndef HUSKY_NO_PAC_SIGN
	pac_install_keys();
	sign_auth_fixups(p, pa_of_va_base);
	con_hex("SIGNED", p->auth_count);
#else
	con_str("SIGNED", "skipped (HUSKY_NO_PAC_SIGN)");
#endif

	/* ADT, then boot_args, behind the kernel; all below topOfKernelData is ours. */
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	copy64(adt_pa, (uintptr_t)p + p->adt_off, p->adt_size);
	/* SMP: secondaries must enter through our EL2 trampoline (start.S). */
	extern char secondary_entry[];
	if (adt_set_u64(adt_pa, p->adt_size, "husky,secondary-entry", (uintptr_t)secondary_entry))
		con_hex("SMPENTRY", (uintptr_t)secondary_entry);
	else
		con_str("SMP", "no husky,secondary-entry in ADT");
	/* UFS: tell xnu's husky_ufs whether ufs_bringup left the host operational. */
	if (!adt_set_u64(adt_pa, p->adt_size, "husky,link-ready", (uint64_t)ufs_ready))
		con_str("UFS", "no husky,link-ready in ADT");
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;

	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = top;
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = p->mem_size;
	/* Boot framebuffer: the panel buffer ABL's splash is scanned out from. */
	uint64_t fb_base;
	uint32_t fb_w, fb_h, fb_pitch;
	if (fb_handoff(&fb_base, &fb_w, &fb_h, &fb_pitch)) {
		ba->Video.v_baseAddr = fb_base;
		ba->Video.v_display = 0;	/* text mode: vcattach shows the verbose console */
		ba->Video.v_rowBytes = fb_pitch;
		ba->Video.v_width = fb_w;
		ba->Video.v_height = fb_h;
		ba->Video.v_depth = 32 | (1u << 16);	/* 32 bpp, 2x scale (kBootVideoDepthScaleShift) */
		con_hex2("FB", "base=", (uint32_t)fb_base, "pitch=", fb_pitch);
	} else {
		con_str("FB", "no boot framebuffer");
	}
	con_hex2("BOOTARG", "pa=", ba_pa, "top=", top);

	uintptr_t entry_pa = p->entry_va + pa_of_va_base;
	con_hex("JUMP", entry_pa);
	con_str("XNU", "jumping");

	/* Everything was written MMU/caches off; drop stale I-cache lines too. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	return "xnu returned";
}
