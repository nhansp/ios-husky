/*
 * UFS probe (UFSPROBE=1 builds only): can we talk to the internal UFS that
 * ABL booted from, without any PHY/UniPro bring-up? Ported from ios-sweet's
 * shim/ufs.c (Qualcomm), retargeted at Tensor G3's Exynos UFSHCI.
 *
 * Runs with the MMU off (all memory Device, so DMA is coherent with no cache
 * maintenance). Dumps the UFSHCI registers ABL left behind and, if the host
 * is enabled and the link is up, points the transfer request list at our own
 * buffers and issues READ-ONLY SCSI commands on LUN 0. Nothing is ever
 * written to the device: there is no SCSI WRITE in this file.
 *
 * Register layout and descriptor formats: JEDEC UFSHCI (JESD223). Exynos
 * hosts (Linux ufs-exynos.c) cap each PRDT entry at 4 KiB
 * (HCI_{TX,RX}PRDT_ENTRY_SIZE = 12) and want HCI_UTRL_NEXUS_TYPE set for SCSI
 * slots; older ones take UTRD offsets in bytes, but zuma (and ABL) use
 * DWORDs. Addresses from zuma-ufs.dtsi, vendor regs from ufs-vs-regs.h.
 */
#include <stdint.h>
#include "console.h"
#include "ufs.h"

#define UFS_BASE	0x13200000UL	/* zuma hci; vendor hci at +0x1100 */

#define REG_CAP		0x00
#define REG_VER		0x08
#define REG_IS		0x20
#define REG_AHIT	0x18	/* auto-hibernate idle timer */
#define REG_HCS		0x30
#define REG_HCE		0x34
#define REG_UTRLBA	0x50
#define REG_UTRLBAU	0x54
#define REG_UTRLDBR	0x58
#define REG_UTRLCLR	0x5c
#define REG_UTRLRSR	0x60
#define REG_UICCMD	0x90
#define REG_UCMDARG1	0x94
#define REG_UCMDARG2	0x98
#define REG_UCMDARG3	0x9c

#define IS_UPMS		(1u << 4)	/* UIC power mode status */
#define IS_UHXS		(1u << 5)	/* UIC hibernate exit status */
#define IS_UHES		(1u << 6)	/* UIC hibernate enter status */
#define IS_UCCS		(1u << 10)	/* UIC command completion status */

#define UIC_DME_GET		0x01
#define UIC_DME_LINKSTARTUP	0x16
#define UIC_DME_HIBER_EXIT	0x18
#define PA_PWRMODE		0x1571
#define PA_CONNECTEDTXLANES	0x1561

#define HCS_DP		(1u << 0)
#define HCS_UTRLRDY	(1u << 1)
#define HCS_UCRDY	(1u << 3)

#define BLOCK		4096u
#define PRDT_MAX	4096u	/* bytes per PRDT entry on Exynos */
#define NPRDT		8	/* up to 32 KiB per command */
#define TICKS_1S	24576000u	/* CNTFRQ on husky */

static inline uint32_t ufs_rd(uint32_t off) { return *(volatile uint32_t *)(UFS_BASE + off); }
static inline void ufs_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(UFS_BASE + off) = v; }

/* Exynos vendor HCI (ufs-vs-regs.h). */
#define VS_BASE			(UFS_BASE + 0x1100)
#define VS_TXPRDT_ENTRY_SIZE	0x00
#define VS_RXPRDT_ENTRY_SIZE	0x04
#define VS_1US_TO_CNT_VAL	0x0C
#define VS_INVALID_UPIU_CTRL	0x10	/* .. 0x24: where a bad descriptor fetch was seen */
#define VS_VENDOR_IS		0x38
#define VS_UTRL_NEXUS_TYPE	0x40
#define VS_UTMRL_NEXUS_TYPE	0x44
#define VS_GPIO_OUT		0x70	/* bit0 drives the device RST_n */
#define VS_SW_RST		0x50
#define VS_DATA_REORDER		0x60
#define VS_AXIDMA_BURST		0x6C
#define VS_REQ_HOLD_EN		0xAC
#define VS_CLKSTOP_CTRL		0xB0
#define VS_FORCE_HCS		0xB4
#define  FORCE_HCS_CORECLK_STOP_EN	(1u << 4)
#define  FORCE_HCS_CLK_STOP_CTRL_ALL	0xde0	/* UFSP DRCG, MPHY APB, refclk(out), unipro p/mclk */
#define VS_FSM_MONITOR		0xC0
#define VS_DMA0_MON_STATE	0xC8
#define VS_DMA1_MON_STATE	0xD0
#define VS_DMA0_DBR_DEBUG	0xD8
#define VS_DMA1_DBR_DEBUG	0xDC
#define VS_AXI_DMA_IF_CTRL	0xF8
#define VS_UFS_ACG_DISABLE	0xFC
#define VS_IOP_ACG_DISABLE	0x100
#define VS_SMU_RD_ABORT		0x118
#define VS_AH8_STATE		0x50C	/* bit16 error, bit8 hibernation, bit0 idle */

#define SYSREG_UFS_IOCC		0x13020710UL	/* sysreg_ufs + 0x710, mask 0x3 */

/* Clock tree (zuma cmucal-sfr.c). Gate: bit20 MANUAL, bit21 CG_VAL, bit28
 * hardware auto-gating. QCH: bit0 ENABLE, bit1 CLOCK_REQ, bit2 IGNORE_FORCE_PM. */
#define CMU_HSI2		0x13000000UL
#define CMU_TOP			0x26040000UL
/* kind 0: dump only, 1: gate (force on), 2: Q-channel (request the clock). */
static void cmu_one(const char *name, uintptr_t addr, int kind)
{
	volatile uint32_t *r = (volatile uint32_t *)addr;
	uint32_t v = *r, nv = v;
	if (kind == 1)
		nv = v | (1u << 20) | (1u << 21);	/* manual, clock on */
	else if (kind == 2)
		nv = v | (1u << 1);			/* CLOCK_REQ */
	if (nv != v)
		*r = nv;
	con_hex2(name, "was=", v, "now=", *r);
}

/* Calls, not a table: pointer tables would need relocation (build.sh). */
static void cmu_dump_and_open(void)
{
	cmu_one("TOP mux", CMU_TOP + 0x10b8, 0);
	cmu_one("TOP div", CMU_TOP + 0x18b0, 0);
	cmu_one("TOP gate", CMU_TOP + 0x20e0, 1);
	cmu_one("noc user", CMU_HSI2 + 0x610, 0);
	cmu_one("ufs user", CMU_HSI2 + 0x630, 0);
	cmu_one("ufs user1", CMU_HSI2 + 0x634, 0);
	cmu_one("qe aclk", CMU_HSI2 + 0x20cc, 1);
	cmu_one("qe pclk", CMU_HSI2 + 0x20d0, 1);
	cmu_one("ufs aclk", CMU_HSI2 + 0x210c, 1);
	cmu_one("ufs unipro", CMU_HSI2 + 0x2110, 1);
	cmu_one("ufs fmp", CMU_HSI2 + 0x2114, 1);
	cmu_one("qch qe", CMU_HSI2 + 0x3090, 2);
	cmu_one("qch ufs", CMU_HSI2 + 0x30c4, 2);
	cmu_one("qch fmp", CMU_HSI2 + 0x30c8, 2);
}
static inline uint32_t vs_rd(uint32_t off) { return *(volatile uint32_t *)(VS_BASE + off); }
static inline void vs_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(VS_BASE + off) = v; }

/* UTP transfer request descriptor (32 bytes). */
struct utrd {
	uint32_t dw0;		/* [31:28] CT, [26:25] DD, [24] I */
	uint32_t dw1;
	uint32_t dw2;		/* [7:0] OCS */
	uint32_t dw3;
	uint32_t ucdba;		/* command descriptor base, 128-byte aligned */
	uint32_t ucdbau;
	uint32_t resp;		/* [31:16] response UPIU offset, [15:0] length */
	uint32_t prdt;		/* [31:16] PRDT offset, [15:0] entries */
};

/* UTP command descriptor: command UPIU, response UPIU, PRDT. */
struct ucd {
	uint8_t cmd[512];
	uint8_t resp[512];
	struct {
		uint32_t addr_lo, addr_hi, rsvd, dbc;	/* dbc = byte count - 1 */
	} prdt[NPRDT];
};

static struct utrd utrl[32] __attribute__((aligned(1024)));
static struct ucd ucd __attribute__((aligned(128)));
static uint8_t data[2 * BLOCK] __attribute__((aligned(4096)));
/* 1: offsets in bytes (older Exynos), 0: in DWORDs (JEDEC; ABL uses these). */
static int byte_gran;

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static void zero(void *p, unsigned n)
{
	volatile uint8_t *b = p;
	for (unsigned i = 0; i < n; i++)
		b[i] = 0;
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void dump_engine(const char *why)
{
	con_hex2(why, "fsm=", vs_rd(VS_FSM_MONITOR), "smu_abort=", vs_rd(VS_SMU_RD_ABORT));
	con_hex2(why, "dma0=", vs_rd(VS_DMA0_MON_STATE), "dma1=", vs_rd(VS_DMA1_MON_STATE));
	con_hex2(why, "dbr0=", vs_rd(VS_DMA0_DBR_DEBUG), "dbr1=", vs_rd(VS_DMA1_DBR_DEBUG));
}

static void dump_errors(const char *why)
{
	dump_engine(why);
	con_hex2(why, "is=", ufs_rd(REG_IS), "hcs=", ufs_rd(REG_HCS));
	con_hex2(why, "dbr=", ufs_rd(REG_UTRLDBR), "vis=", vs_rd(VS_VENDOR_IS));
	con_hex2(why, "inv_ctrl=", vs_rd(VS_INVALID_UPIU_CTRL), "inv_badr=", vs_rd(VS_INVALID_UPIU_CTRL + 4));
	con_hex2(why, "inv_utr=", vs_rd(0x20), "inv_din=", vs_rd(0x24));
}

/*
 * Issue one SCSI command (read direction or no data) in list slot slotno,
 * whose descriptor is at slot, with the command descriptor at ucdp: command UPIU at 0, response
 * UPIU at resp_off, PRDT at prdt_off (bytes). Returns 0 on success, otherwise
 * a code describing the failure stage.
 */
static uint32_t scsi_at(unsigned slotno, volatile struct utrd *slot, uint8_t *ucdp, uint32_t resp_off, uint32_t resp_len,
    uint32_t prdt_off, uint8_t lun, const uint8_t *cdb, unsigned cdb_len, void *buf, uint32_t len,
    uint32_t *ocs_out, uint32_t *status_out)
{
	static uint8_t tag;
	unsigned unit = byte_gran ? 1 : 4;
	if (len > NPRDT * PRDT_MAX)
		return 5;
	zero((void *)slot, sizeof(*slot));
	zero(ucdp, prdt_off + NPRDT * 16);

	/* Command UPIU: header (12 bytes), expected length, CDB. */
	volatile uint8_t *cmd = ucdp;
	cmd[0] = 0x01;				/* transaction type: COMMAND */
	cmd[1] = len ? 0x40 : 0x00;		/* flags: R (data in) */
	cmd[2] = lun;
	cmd[3] = ++tag;
	cmd[12] = len >> 24;
	cmd[13] = len >> 16;
	cmd[14] = len >> 8;
	cmd[15] = len;
	for (unsigned i = 0; i < cdb_len; i++)
		cmd[16 + i] = cdb[i];

	uintptr_t ucd_pa = (uintptr_t)ucdp;
	slot->dw0 = (1u << 28) | ((len ? 2u : 0u) << 25) | (1u << 24);	/* UFS storage, dev->host */
	slot->dw2 = 0x0f;			/* OCS: invalid until the host updates it */
	slot->ucdba = (uint32_t)ucd_pa;
	slot->ucdbau = (uint32_t)(ucd_pa >> 32);
	slot->resp = (resp_off / unit) << 16 | (resp_len / unit);
	if (len) {
		volatile uint32_t *prdt = (volatile uint32_t *)(ucdp + prdt_off);
		uintptr_t buf_pa = (uintptr_t)buf;
		unsigned n = 0;
		for (uint32_t off = 0; off < len; off += PRDT_MAX, n++) {
			uint32_t chunk = len - off < PRDT_MAX ? len - off : PRDT_MAX;
			prdt[4 * n + 0] = (uint32_t)(buf_pa + off);
			prdt[4 * n + 1] = (uint32_t)((buf_pa + off) >> 32);
			prdt[4 * n + 3] = chunk - 1;
		}
		slot->prdt = (prdt_off / unit) << 16 | n;
	}
	__asm__ volatile("dsb sy" ::: "memory");

	uint32_t bit = 1u << slotno;
	vs_wr(VS_UTRL_NEXUS_TYPE, vs_rd(VS_UTRL_NEXUS_TYPE) | bit);	/* a SCSI command */
	ufs_wr(REG_UTRLDBR, bit);
	uint64_t start = ticks();
	while (ufs_rd(REG_UTRLDBR) & bit) {
		if (ticks() - start > TICKS_1S) {
			dump_errors("UFS TMO");
			ufs_wr(REG_UTRLCLR, ~bit);	/* clear the slot (write 0 to its bit) */
			return 1;			/* timeout */
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	*ocs_out = slot->dw2 & 0xff;
	*status_out = ucdp[resp_off + 7];	/* SCSI status in the response UPIU */
	if (*ocs_out) {
		dump_errors("UFS OCS");
		return 2;
	}
	if (ucdp[resp_off] != 0x21)		/* RESPONSE UPIU */
		return 3;
	return *status_out ? 4 : 0;
}

static uint32_t scsi(uint8_t lun, const uint8_t *cdb, unsigned cdb_len, void *buf, uint32_t len,
    uint32_t *ocs_out, uint32_t *status_out)
{
	return scsi_at(0, &utrl[0], (uint8_t *)&ucd, 512, 512, 1024, lun, cdb, cdb_len, buf, len,
	    ocs_out, status_out);
}

static uint32_t read_blocks(uint32_t lba, uint32_t count, void *buf)
{
	uint8_t cdb[10] = { 0x28, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, count >> 8, count, 0 };
	uint32_t ocs, st;
	return scsi(0, cdb, sizeof(cdb), buf, count * BLOCK, &ocs, &st);
}

static uint64_t le64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

/* One line per GPT entry of at least 64 MiB: name, first LBA, size in MiB (4K blocks). */
static void list_partitions(uint32_t entry_lba, uint32_t nent)
{
	static uint8_t ents[128 * 128] __attribute__((aligned(4096)));
	uint32_t blocks = (nent * 128 + BLOCK - 1) / BLOCK;
	for (uint32_t b = 0; b < blocks; b += NPRDT) {
		uint32_t n = blocks - b < NPRDT ? blocks - b : NPRDT;
		if (read_blocks(entry_lba + b, n, ents + b * BLOCK)) {
			con_str("UFS PART", "read error");
			return;
		}
	}
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = ents + i * 128;
		uint64_t first = le64(e + 32), last = le64(e + 40);
		if (!first)
			continue;
		char name[17];
		for (int c = 0; c < 16; c++) {
			uint8_t ch = e[56 + 2 * c];
			name[c] = (ch >= 32 && ch < 127) ? ch : (ch ? '?' : 0);
		}
		name[16] = 0;
		if (((last - first + 1) >> 8) < 64)	/* keep the log to the big ones */
			continue;
		con_hex2(name, "lba=", (uint32_t)first, "MiB=", (uint32_t)((last - first + 1) >> 8));
	}
}

/*
 * One UIC command. Returns the config result code (ARG2[7:0], 0 = success)
 * or 0x100 on timeout; *val gets ARG3 (DME_GET's value). IS bits it raised
 * are returned in *is.
 */
static uint32_t uic_cmd(uint32_t op, uint32_t arg1, uint32_t *val, uint32_t *is)
{
	uint64_t start = ticks();
	while (!(ufs_rd(REG_HCS) & HCS_UCRDY))
		if (ticks() - start > TICKS_1S)
			return 0x101;
	ufs_wr(REG_IS, IS_UCCS | IS_UHXS | IS_UHES | IS_UPMS);
	ufs_wr(REG_UCMDARG1, arg1);
	ufs_wr(REG_UCMDARG2, 0);
	ufs_wr(REG_UCMDARG3, 0);
	ufs_wr(REG_UICCMD, op);
	start = ticks();
	uint32_t want = op == UIC_DME_HIBER_EXIT ? IS_UHXS : IS_UCCS;
	while (!(ufs_rd(REG_IS) & want))
		if (ticks() - start > TICKS_1S) {
			*is = ufs_rd(REG_IS);
			return 0x100;
		}
	*is = ufs_rd(REG_IS);
	ufs_wr(REG_IS, *is & (IS_UCCS | IS_UHXS | IS_UHES | IS_UPMS));
	if (val)
		*val = ufs_rd(REG_UCMDARG3);
	return ufs_rd(REG_UCMDARG2) & 0xff;
}

static void dme_get_log(const char *name, uint32_t attr)
{
	uint32_t val = 0, is = 0;
	uint32_t rc = uic_cmd(UIC_DME_GET, attr << 16, &val, &is);
	con_hex2(name, "rc=", rc, "val=", val);
}

static void delay_us(uint32_t us)
{
	uint64_t start = ticks(), n = (uint64_t)us * (TICKS_1S / 1000000u);
	while (ticks() - start < n)
		;
}

/* Submit a prebuilt no-data UPIU (in ucd.cmd) from our slot 0; returns
 * 0 when the host completed it with OCS 0, else a stage code. */
static uint32_t upiu_nodata(uint32_t ct_dd)
{
	unsigned unit = byte_gran ? 1 : 4;
	zero(&utrl[0], sizeof(utrl[0]));
	utrl[0].dw0 = ct_dd | (1u << 24);
	utrl[0].dw2 = 0x0f;
	utrl[0].ucdba = (uint32_t)(uintptr_t)&ucd;
	utrl[0].ucdbau = 0;
	utrl[0].resp = (512u / unit) << 16 | (512u / unit);
	__asm__ volatile("dsb sy" ::: "memory");
	vs_wr(VS_UTRL_NEXUS_TYPE, vs_rd(VS_UTRL_NEXUS_TYPE) & ~1u);	/* not a SCSI command */
	ufs_wr(REG_UTRLDBR, 1);
	uint64_t start = ticks();
	while (ufs_rd(REG_UTRLDBR) & 1)
		if (ticks() - start > TICKS_1S) {
			dump_errors("UFS TMO");
			ufs_wr(REG_UTRLCLR, ~1u);
			return 1;
		}
	__asm__ volatile("dsb sy" ::: "memory");
	return (utrl[0].dw2 & 0xff) ? 2 : 0;
}

/* QUERY REQUEST (standard flag op on idn); returns the flag value or -code. */
static int query_flag(uint8_t opcode, uint8_t idn)
{
	static uint8_t qtag;
	zero(&ucd, 1024);
	ucd.cmd[0] = 0x16;			/* QUERY REQUEST */
	ucd.cmd[3] = ++qtag;
	ucd.cmd[5] = opcode == 0x05 ? 0x01 : 0x81;	/* standard read / write request */
	ucd.cmd[12] = opcode;			/* 0x05 read flag, 0x06 set flag */
	ucd.cmd[13] = idn;
	uint32_t rc = upiu_nodata(0);
	if (rc)
		return -(int)rc;
	if (ucd.resp[0] != 0x36)		/* QUERY RESPONSE */
		return -3;
	if (ucd.resp[6])			/* query response code */
		return -(0x10 + ucd.resp[6]);
	return ucd.resp[12 + 11];		/* flag value: last byte of the 16-byte area */
}

/*
 * Reset the device and host the way Linux's hce_enable_notify does, start the
 * link without the Samsung CAL tables (betting ABL's PHY setup survives), then
 * run the UFS device init (NOP OUT, fDeviceInit). Returns 0 when the device
 * is ready for SCSI commands from our list.
 */
static int host_reset_and_link(void)
{
	con_str("UFS RESET", "device + host reset, link startup (no CAL)");
	vs_wr(VS_GPIO_OUT, 0);			/* RST_n low */
	delay_us(10);
	vs_wr(VS_GPIO_OUT, 1);
	delay_us(1000);
	ufs_wr(REG_HCE, 0);
	uint64_t start = ticks();
	while (ufs_rd(REG_HCE) & 1)
		if (ticks() - start > TICKS_1S / 10) {
			con_str("UFS RESET", "HCE stuck at 1");
			return 1;
		}
	vs_wr(VS_DATA_REORDER, 0xA);
	vs_wr(VS_TXPRDT_ENTRY_SIZE, (1u << 31) | 12);
	vs_wr(VS_RXPRDT_ENTRY_SIZE, 12);
	vs_wr(VS_UTRL_NEXUS_TYPE, ~0u);
	vs_wr(VS_UTMRL_NEXUS_TYPE, ~0u);
	ufs_wr(REG_HCE, 1);
	start = ticks();
	while (!(ufs_rd(REG_HCE) & 1))
		if (ticks() - start > TICKS_1S / 10) {
			con_str("UFS RESET", "HCE never set");
			return 2;
		}
	con_hex2("UFS RESET", "hce=", ufs_rd(REG_HCE), "hcs=", ufs_rd(REG_HCS));
	uint32_t rc = 0x100, is = 0;
	for (int i = 0; i < 3 && rc; i++) {
		rc = uic_cmd(UIC_DME_LINKSTARTUP, 0, 0, &is);
		con_hex2("UFS LINKUP", "rc=", rc, "is=", is);
		con_hex2("UFS LINKUP", "hcs=", ufs_rd(REG_HCS), "dp=", ufs_rd(REG_HCS) & HCS_DP);
		if (!rc && !(ufs_rd(REG_HCS) & HCS_DP))
			rc = 0x200;
	}
	if (rc)
		return 3;
	dme_get_log("UFS PWRMODE", PA_PWRMODE);
	ufs_wr(REG_IS, ufs_rd(REG_IS));
	/* Our list, running. */
	ufs_wr(REG_UTRLBA, (uint32_t)(uintptr_t)utrl);
	ufs_wr(REG_UTRLBAU, 0);
	ufs_wr(REG_UTRLRSR, 1);
	con_hex2("UFS RESET", "utrlba=", ufs_rd(REG_UTRLBA), "rsr=", ufs_rd(REG_UTRLRSR));
	/* NOP OUT until the device answers NOP IN. */
	for (int i = 0; i < 10; i++) {
		zero(&ucd, 1024);
		ucd.cmd[0] = 0x00;		/* NOP OUT */
		ucd.cmd[3] = 0x50 + i;
		rc = upiu_nodata(0);
		con_hex2("UFS NOP", "rc=", rc, "resp=", ucd.resp[0]);
		if (!rc && ucd.resp[0] == 0x20)	/* NOP IN */
			break;
	}
	if (rc)
		return 4;
	int f = query_flag(0x06, 0x01);		/* set fDeviceInit */
	con_hex("UFS fDevInit set", (uint32_t)f);
	for (int i = 0; i < 200 && f != 0; i++) {
		delay_us(5000);
		f = query_flag(0x05, 0x01);
	}
	con_hex("UFS fDevInit", (uint32_t)f);
	return f == 0 ? 0 : 5;
}

void test_ufs(void)
{
	con_str("UFS", "probe v8 (read-only)");
	uint32_t cap = ufs_rd(REG_CAP), hcs = ufs_rd(REG_HCS), hce = ufs_rd(REG_HCE);
	con_hex2("UFS", "ver=", ufs_rd(REG_VER), "cap=", cap);
	con_hex2("UFS", "hce=", hce, "hcs=", hcs);
	con_hex2("UFS", "is=", ufs_rd(REG_IS), "rsr=", ufs_rd(REG_UTRLRSR));
	uint32_t abl_lo = ufs_rd(REG_UTRLBA), abl_hi = ufs_rd(REG_UTRLBAU);
	con_hex2("UFS", "utrlba=", abl_lo, "hi=", abl_hi);
	con_hex2("UFS", "dbr=", ufs_rd(REG_UTRLDBR), "nutrs=", (cap & 0x1f) + 1);
	con_hex2("UFS VS", "txprdt=", vs_rd(VS_TXPRDT_ENTRY_SIZE), "rxprdt=", vs_rd(VS_RXPRDT_ENTRY_SIZE));
	con_hex2("UFS VS", "nexus=", vs_rd(VS_UTRL_NEXUS_TYPE), "1us=", vs_rd(VS_1US_TO_CNT_VAL));
	con_hex2("UFS VS", "hold=", vs_rd(VS_REQ_HOLD_EN), "clkstop=", vs_rd(VS_CLKSTOP_CTRL));
	con_hex2("UFS VS", "reorder=", vs_rd(VS_DATA_REORDER), "burst=", vs_rd(VS_AXIDMA_BURST));
	con_hex2("UFS VS", "swrst=", vs_rd(VS_SW_RST), "vis=", vs_rd(VS_VENDOR_IS));
	/* ABL's own slot 0 shows which descriptor layout this host expects. */
	volatile struct utrd *abl = 0;
	uint8_t *abl_ucd = 0;
	if (abl_lo && !abl_hi) {
		abl = (volatile struct utrd *)(uintptr_t)abl_lo;
		con_hex2("UFS ABL", "dw0=", abl->dw0, "dw2=", abl->dw2);
		con_hex2("UFS ABL", "ucdba=", abl->ucdba, "hi=", abl->ucdbau);
		con_hex2("UFS ABL", "resp=", abl->resp, "prdt=", abl->prdt);
		if (abl->ucdba && !abl->ucdbau) {
			abl_ucd = (uint8_t *)(uintptr_t)abl->ucdba;
			volatile const uint32_t *u = (volatile const uint32_t *)abl_ucd;
			con_hex2("UFS ABL cmd", "w0=", u[0], "w1=", u[1]);
			con_hex2("UFS ABL cmd", "w2=", u[2], "w4=", u[4]);
		}
	}

	if (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {
		con_str("UFS", "host not enabled/ready: stop");
		return;
	}
	if (ufs_rd(REG_UTRLDBR)) {
		con_str("UFS", "requests outstanding: stop");
		return;
	}
	/* Requests held or host clocks stopped would leave a doorbell pending forever. */
	if (vs_rd(VS_REQ_HOLD_EN)) {
		vs_wr(VS_REQ_HOLD_EN, 0);
		con_hex("UFS hold clr", vs_rd(VS_REQ_HOLD_EN));
	}
	if (vs_rd(VS_CLKSTOP_CTRL) & 0x1f) {
		vs_wr(VS_CLKSTOP_CTRL, vs_rd(VS_CLKSTOP_CTRL) & ~0x1fu);
		con_hex("UFS clkstop clr", vs_rd(VS_CLKSTOP_CTRL));
	}
	ufs_wr(REG_IS, ufs_rd(REG_IS));		/* ack stale status (write 1 to clear) */

	/* Link state ABL left: power mode, lanes. Then leave hibern8 if it is in it. */
	dme_get_log("UFS PWRMODE", PA_PWRMODE);
	dme_get_log("UFS TXLANES", PA_CONNECTEDTXLANES);
	/* Clock/DMA plumbing ABL left, then Linux's settings for an active host:
	 * core clock never auto-stopped, no clock-stop control, IOP auto clock
	 * gating enabled (exynos_ufs_ctrl_auto_hci_clk/ctrl_clk(false)). */
	con_hex2("UFS VS", "force_hcs=", vs_rd(VS_FORCE_HCS), "axi_if=", vs_rd(VS_AXI_DMA_IF_CTRL));
	con_hex2("UFS VS", "ufs_acg=", vs_rd(VS_UFS_ACG_DISABLE), "iop_acg=", vs_rd(VS_IOP_ACG_DISABLE));
	dump_engine("UFS IDLE");
	vs_wr(VS_FORCE_HCS, vs_rd(VS_FORCE_HCS) & ~(FORCE_HCS_CORECLK_STOP_EN | FORCE_HCS_CLK_STOP_CTRL_ALL));
	vs_wr(VS_IOP_ACG_DISABLE, vs_rd(VS_IOP_ACG_DISABLE) & ~1u);
	con_hex2("UFS VS", "force_hcs'=", vs_rd(VS_FORCE_HCS), "iop_acg'=", vs_rd(VS_IOP_ACG_DISABLE));

	uint32_t ocs = 0, st = 0, rc = 1;
	static const uint8_t tur[6] = { 0x00 };

	/*
	 * Clocks were checked (v6: all UFS gates on, Q-channels requested).
	 * Pixel runs the host's own auto-hibern8 (10 ms): if ABL left that FSM
	 * hibernating or in error, requests wait forever. PA_PWRMODE still
	 * reads FAST in hibern8, so ask the host, not UniPro.
	 */
	con_hex2("UFS AH8", "ahit=", ufs_rd(REG_AHIT), "state=", vs_rd(VS_AH8_STATE));
	ufs_wr(REG_AHIT, 0);
	con_hex2("UFS AH8", "ahit'=", ufs_rd(REG_AHIT), "state'=", vs_rd(VS_AH8_STATE));
	if (vs_rd(VS_AH8_STATE) & (1u << 8)) {
		uint32_t is = 0;
		uint32_t rc8 = uic_cmd(UIC_DME_HIBER_EXIT, 0, 0, &is);
		con_hex2("UFS H8EXIT", "rc=", rc8, "is=", is);
		con_hex2("UFS H8EXIT", "upmcrs=", (ufs_rd(REG_HCS) >> 8) & 7, "state=", vs_rd(VS_AH8_STATE));
	}

	/*
	 * First, no takeover at all: ABL's list stays installed and running; our
	 * TUR goes in its slot 1 (ABL used slot 0) with a command descriptor in
	 * ABL's memory, doorbell bit 1. Isolates the list stop/rebase from
	 * everything else.
	 */
	if (abl && abl_ucd) {
		volatile struct utrd *s1 = abl + 1;
		uint8_t *u1 = (uint8_t *)(((uintptr_t)abl_ucd + 0x800) & ~(uintptr_t)0x7f);
		con_hex2("UFS NOTAKE", "slot1=", (uint32_t)(uintptr_t)s1, "ucd=", (uint32_t)(uintptr_t)u1);
		for (int i = 0; i < 3; i++) {
			rc = scsi_at(1, s1, u1, 32, 32, 64, 0, tur, sizeof(tur), 0, 0, &ocs, &st);
			con_hex2("UFS TUR slot1", "rc=", rc, "ocs/st=", ocs << 8 | st);
			if (!rc || rc == 4)
				break;
		}
		if (!rc || rc == 4) {
			con_str("UFS", "ABL list works untouched: the takeover breaks it");
			return;
		}
	}

	/* Take over the transfer request list: stop, point at ours, run. */
	ufs_wr(REG_UTRLRSR, 0);
	uintptr_t list = (uintptr_t)utrl;
	ufs_wr(REG_UTRLBA, (uint32_t)list);
	ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	con_hex2("UFS", "ours=", (uint32_t)list, "rsr=", ufs_rd(REG_UTRLRSR));
	/*
	 * Never ring a doorbell unless the host really uses our list: otherwise
	 * it would execute whatever ABL left in its own slot 0 (maybe a write).
	 */
	if (ufs_rd(REG_UTRLBA) != (uint32_t)list || ufs_rd(REG_UTRLBAU) != (uint32_t)(list >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1)) {
		con_str("UFS", "list base not taken: stop");
		return;
	}
	/* A first command may report a unit attention (CHECK CONDITION): retry. */
	for (int i = 0; i < 3; i++) {
		rc = scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st);
		con_hex2("UFS TUR ours", "rc=", rc, "ocs/st=", ocs << 8 | st);
		if (!rc || rc == 4)
			break;
	}

	/* Last: sysreg / protector blocks may be secure-only and fault. */
	if ((rc == 1 || rc == 2) && abl && abl_ucd) {
		/*
		 * Not even a no-data command completes from our buffers: retry from
		 * ABL's list and command descriptor, which the host's DMA demonstrably
		 * reaches. Same layout ABL used (response UPIU right after the 32-byte
		 * command UPIU). If this works where ours failed, DMA to our memory is
		 * being blocked (S2MPU).
		 */
		ufs_wr(REG_UTRLRSR, 0);
		ufs_wr(REG_UTRLBA, abl_lo);
		ufs_wr(REG_UTRLBAU, 0);
		ufs_wr(REG_UTRLRSR, 1);
		con_hex2("UFS", "abl list=", ufs_rd(REG_UTRLBA), "rsr=", ufs_rd(REG_UTRLRSR));
		for (int i = 0; i < 3; i++) {
			rc = scsi_at(0, abl, abl_ucd, 32, 32, 64, 0, tur, sizeof(tur), 0, 0, &ocs, &st);
			con_hex2("UFS TUR abl", "rc=", rc, "ocs/st=", ocs << 8 | st);
			if (!rc || rc == 4)
				break;
		}
		con_str("UFS", rc == 0 || rc == 4 ? "ABL memory works, ours does not: DMA blocked?"
		    : "fails in ABL memory too: not (only) DMA");
		if (rc == 0 || rc == 4)
			return;
		/* Do what Linux always does: reset everything and start the link. */
		int r = host_reset_and_link();
		con_hex("UFS RESET result", (uint32_t)r);
		if (r)
			return;
		for (int i = 0; i < 3; i++) {
			rc = scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st);
			con_hex2("UFS TUR reset", "rc=", rc, "ocs/st=", ocs << 8 | st);
			if (!rc || rc == 4)
				break;
		}
	}
	if (rc == 4)
		rc = 0;
	if (rc) {
		con_str("UFS", "no TUR response: stop");
		return;
	}

	static const uint8_t cap10[10] = { 0x25 };	/* READ CAPACITY(10) */
	rc = scsi(0, cap10, sizeof(cap10), data, 8, &ocs, &st);
	con_hex2("UFS CAP", "rc=", rc, "ocs/st=", ocs << 8 | st);
	if (!rc)
		con_hex2("UFS LUN0", "lastlba=", be32(data), "blk=", be32(data + 4));

	/* READ(10) LBA 0, 2 blocks: protective MBR and the GPT header. */
	static const uint8_t rd10[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 2, 0 };
	rc = scsi(0, rd10, sizeof(rd10), data, 2 * BLOCK, &ocs, &st);
	con_hex2("UFS READ", "rc=", rc, "ocs/st=", ocs << 8 | st);
	if (rc)
		return;
	con_hex2("UFS MBR", "sig=", data[510] | data[511] << 8, "type=", data[450]);
	const uint8_t *gpt = data + BLOCK;
	char sig[9];
	for (int i = 0; i < 8; i++)
		sig[i] = (gpt[i] >= 32 && gpt[i] < 127) ? gpt[i] : '.';
	sig[8] = 0;
	con_str("UFS GPT", sig);
	uint32_t nent = gpt[80] | gpt[81] << 8 | gpt[82] << 16, esz = gpt[84] | gpt[85] << 8;
	uint32_t elba = gpt[72] | gpt[73] << 8;
	con_hex2("UFS GPT", "entries=", nent, "lba=", elba);
	if (esz != 128 || nent > 128)
		return;
	list_partitions(elba, nent);
}
