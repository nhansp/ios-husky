/*
 * Boot framebuffer for xnu: hand over the buffer ABL's splash is scanned out
 * from, so xnu's console draws on the panel. FBPROBE=1 builds add a probe
 * that logs every layer and paints test stripes.
 *
 * ABL leaves DECON0 (0x19470000) running after the handoff: the splash stays
 * on the panel. Each DPP layer has an RDMA block (zuma regs-dpp.h, windows
 * from zuma-base.dts drmdpp@...) holding the source size, format, stride and
 * base address. Reads only, except: if an enabled layer's base looks like
 * physical DRAM, paint a stripe across the top of it. A stripe appearing on
 * the panel proves the address is the live scanout (not an IOMMU address).
 */
#include <stdint.h>
#include "console.h"
#include "fb.h"

#define RDMA_ENABLE		0x00
#define RDMA_IN_CTRL_0		0x08	/* [13:8] IMG_FORMAT */
#define RDMA_SRC_WIDTH		0x10
#define RDMA_SRC_HEIGHT		0x14
#define RDMA_IMG_SIZE		0x1C
#define RDMA_BASEADDR_P0	0x40
#define RDMA_SRC_STRIDE_0	0x50

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }

static inline uint64_t cntvct(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

/* DRAM banks on husky: low bank from 0x80000000, high banks from 0x880000000. */
static int looks_like_dram(uint64_t pa)
{
	return (pa >= 0x80000000ULL && pa < 0x100000000ULL) || pa >= 0x880000000ULL;
}

static uint64_t live_base;
static uint32_t live_pitch, live_w;

static void paint(uint32_t y0, uint32_t rows, uint32_t argb)
{
	for (uint32_t y = y0; y < y0 + rows; y++) {
		volatile uint32_t *row = (volatile uint32_t *)(uintptr_t)(live_base + (uint64_t)y * live_pitch);
		for (uint32_t x = 0; x < live_w; x++)
			row[x] = argb;
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

static void probe_layer(uintptr_t dma)
{
	uint32_t en = rd(dma + RDMA_ENABLE), ctrl = rd(dma + RDMA_IN_CTRL_0);
	uint32_t w = rd(dma + RDMA_SRC_WIDTH), h = rd(dma + RDMA_SRC_HEIGHT);
	uint32_t img = rd(dma + RDMA_IMG_SIZE), stride = rd(dma + RDMA_SRC_STRIDE_0);
	uint32_t base = rd(dma + RDMA_BASEADDR_P0);
	con_hex2("FB DPP", "dma=", (uint32_t)dma, "en=", en);
	if (!en && !base)
		return;
	con_hex2("FB DPP", "ctrl=", ctrl, "fmt=", (ctrl >> 8) & 0x3f);
	con_hex2("FB DPP", "srcw=", w, "srch=", h);
	con_hex2("FB DPP", "img=", img, "stride=", stride);
	con_hex2("FB DPP", "base=", base, "dram=", looks_like_dram(base));
	/* RDMA_ENABLE reads 0x40000000 on the live splash layer, not bit 0: go by
	 * a programmed size and a DRAM base instead. */
	if (!(w & 0xffff) || !looks_like_dram(base))
		return;
	/*
	 * Stripe: 48 rows of opaque magenta across the top. Stride is taken
	 * from the register if set, else 4 bytes x width.
	 */
	uint32_t pitch = (stride & 0xffff) ? (stride & 0xffff) : w * 4;
	uint32_t wpx = w & 0xffff;
	if (!wpx || wpx > 4096 || pitch < wpx * 4)
		pitch = 1344 * 4, wpx = 1344;
	if (!live_base) {
		live_base = base;
		live_pitch = pitch;
		live_w = wpx;
	}
	con_hex2("FB LIVE", "base=", base, "pitch=", pitch);
}

#define DECON0			0x19470000UL
#define DECON_GLOBAL_CON	0x20	/* bit8 command mode, bit5 idle, bit4 run, bit1/0 enable */
#define DECON_TRIG_CON		0x30	/* bit8 SW_TRIG_EN, bit4 HW_TRIG_MASK_DECON, bit1 SW_TRIG_DET_EN, bit0 HW_TRIG_EN */
#define DECON_SHD_REG_UP_REQ	0x50	/* bit31 global, [13:0] windows */
#define SYSMMU_DPU0		0x19840000UL	/* samsung,sysmmu-v9 "DPU F0" */

void test_fb(void)
{
	con_str("FB", "display probe v3");
	uint32_t gcon = rd(DECON0 + DECON_GLOBAL_CON), trig = rd(DECON0 + DECON_TRIG_CON);
	con_hex2("FB DECON0", "global=", gcon, "trig=", trig);
	con_hex2("FB DECON0", "cmd_mode=", (gcon >> 8) & 1, "run=", (gcon >> 4) & 1);
	/* Is translation on? (v9 layout unknown here: dump the likely control words.) */
	con_hex2("FB SMMU", "0x0=", rd(SYSMMU_DPU0), "0x4=", rd(SYSMMU_DPU0 + 4));
	con_hex2("FB SMMU", "0x8=", rd(SYSMMU_DPU0 + 8), "0xc=", rd(SYSMMU_DPU0 + 0xc));
	con_hex2("FB SMMU", "0x8000=", rd(SYSMMU_DPU0 + 0x8000), "0x8004=", rd(SYSMMU_DPU0 + 0x8004));
	con_hex2("FB SMMU", "0x8008=", rd(SYSMMU_DPU0 + 0x8008), "0x800c=", rd(SYSMMU_DPU0 + 0x800c));
	/*
	 * Command-mode panels keep showing their last frame; the DECON only
	 * sends a new one on the panel's TE while the HW trigger is unmasked
	 * (Linux decon_reg_set_trigger). Unmask it so buffer writes reach the
	 * panel on every TE.
	 */
	/* DPP RDMA windows, from zuma-base.dts drmdpp@... (first reg = "dma"). */
	for (uintptr_t a = 0x19900000; a <= 0x19906000; a += 0x1000)
		probe_layer(a);
	for (uintptr_t a = 0x19D00000; a <= 0x19D06000; a += 0x1000)
		probe_layer(a);
	probe_layer(0x19908000);
	probe_layer(0x1990C000);
	probe_layer(0x19D0C000);

	if (!live_base || !((gcon >> 8) & 1)) {
		con_str("FB", "no live layer / not command mode: done");
		return;
	}
	volatile uint32_t *g = (volatile uint32_t *)(DECON0 + DECON_GLOBAL_CON);
	volatile uint32_t *t = (volatile uint32_t *)(DECON0 + DECON_TRIG_CON);
	volatile uint32_t *u = (volatile uint32_t *)(DECON0 + DECON_SHD_REG_UP_REQ);
	uint64_t t0;

	/* Phase 1 (magenta, rows 0-63): shadow update + TE-driven HW trigger. */
	paint(0, 64, 0xffff00ffu);
	*g |= 3;					/* DECON_EN | DECON_EN_F */
	*u |= (1u << 31) | 0x3fff;			/* global + all windows */
	*t = (*t | 1u) & ~(1u << 4);			/* HW_TRIG_EN, unmasked */
	__asm__ volatile("dsb sy" ::: "memory");
	t0 = cntvct();
	while (cntvct() - t0 < 24576000u / 10)
		;
	con_hex2("FB HW", "trig=", *t, "upreq=", *u);
	con_hex2("FB HW", "global=", *g, "run=", (*g >> 4) & 1);

	/* Phase 2 (green, rows 64-127): shadow update + SW trigger. */
	t0 = cntvct();
	while (cntvct() - t0 < 24576000u / 5)
		;
	paint(64, 64, 0xff00ff00u);
	*t = (*t & ~1u) | (1u << 4);			/* HW trigger off/masked */
	*g |= 3;
	*u |= (1u << 31) | 0x3fff;
	*t |= (1u << 8) | (1u << 1);			/* SW_TRIG_EN | SW_TRIG_DET_EN */
	__asm__ volatile("dsb sy" ::: "memory");
	t0 = cntvct();
	while (cntvct() - t0 < 24576000u / 10)
		;
	con_hex2("FB SW", "trig=", *t, "upreq=", *u);
	con_hex2("FB SW", "global=", *g, "run=", (*g >> 4) & 1);
}

/*
 * Always-on handoff. DPP0's RDMA (0x19900000) is the splash layer: the probe
 * showed 1344x2992 BGRA8888 at a physical base (DPU SysMMU in bypass). The
 * panel is in DSI command mode and the DECON idles after ABL's single frame,
 * so enable its TE-driven trigger unmasked: every TE then pushes the buffer,
 * and whatever xnu draws reaches the panel. Returns 0 if no usable layer.
 */
int fb_handoff(uint64_t *base, uint32_t *width, uint32_t *height, uint32_t *pitch)
{
	uintptr_t dma = 0x19900000;
	uint32_t w = rd(dma + RDMA_SRC_WIDTH) & 0xffff, h = rd(dma + RDMA_SRC_HEIGHT) & 0xffff;
	uint32_t fmt = (rd(dma + RDMA_IN_CTRL_0) >> 8) & 0x3f, stride = rd(dma + RDMA_SRC_STRIDE_0) & 0xffff;
	uint64_t b = rd(dma + RDMA_BASEADDR_P0);
	if (!w || !h || w > 4096 || h > 4096 || fmt != 0 || !looks_like_dram(b))
		return 0;
	uint32_t gcon = rd(DECON0 + DECON_GLOBAL_CON);
	if ((gcon >> 8) & 1) {
		volatile uint32_t *t = (volatile uint32_t *)(DECON0 + DECON_TRIG_CON);
		*(volatile uint32_t *)(DECON0 + DECON_GLOBAL_CON) = gcon | 3;	/* DECON_EN | DECON_EN_F */
		*(volatile uint32_t *)(DECON0 + DECON_SHD_REG_UP_REQ) |= (1u << 31) | 0x3fff;
		*t = (*t | 1u) & ~(1u << 4);		/* HW_TRIG_EN, unmasked */
		__asm__ volatile("dsb sy" ::: "memory");
	}
	*base = b;
	*width = w;
	*height = h;
	*pitch = stride ? stride : w * 4;
	return 1;
}
