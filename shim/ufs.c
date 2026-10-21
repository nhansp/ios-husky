/*
 * Internal UFS bring-up for xnu. ABL powers the UFS down (VCC off, REF_CLK
 * and RST_n parked) before the handoff, so the shim brings host, link and
 * device back up the way Linux's ufshcd + ufs-exynos do, with Samsung's zuma
 * CAL (shim/ufscal, GPL) for the PHY/UniPro tables, switches the link to
 * HS-G4 and leaves it operational. xnu's bsd/dev/husky_ufs.c then takes over
 * the transfer request list; the CAL never has to live in the kernel.
 *
 * Runs with the MMU off (all memory Device, so DMA is coherent with no cache
 * maintenance). Nothing is ever written to the device: there is no SCSI WRITE
 * in this file. UFSPROBE=1 builds also log capacity, the GPT and a read
 * throughput sample.
 *
 * Register layout and descriptor formats: JEDEC UFSHCI (JESD223). Exynos
 * hosts (Linux ufs-exynos.c) cap each PRDT entry at 4 KiB
 * (HCI_{TX,RX}PRDT_ENTRY_SIZE = 12) and want HCI_UTRL_NEXUS_TYPE set for SCSI
 * slots; zuma (and ABL) take UTRD offsets in DWORDs. Addresses from
 * zuma-ufs.dtsi, vendor regs from ufs-vs-regs.h.
 */
#include <stdint.h>
#include "console.h"
#include "ufs.h"
/* Samsung's zuma UFS CAL (shim/ufscal, vendored from Google's GPL kernel). */
#include "linux/types.h"
#include "linux/io.h"
#include "ufs-vs-mmio.h"
#include "ufs-cal-if.h"

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
#define UIC_DME_SET		0x02
#define UIC_DME_LINKSTARTUP	0x16
#define UIC_DME_ENDPOINTRESET	0x15
#define UIC_DME_HIBER_EXIT	0x18
#define PA_ACTIVETXDATALANES	0x1560
#define PA_CONNECTEDTXLANES	0x1561
#define PA_TXGEAR		0x1568
#define PA_TXTERMINATION	0x1569
#define PA_HSSERIES		0x156A
#define PA_PWRMODE		0x1571
#define PA_ACTIVERXDATALANES	0x1580
#define PA_RXGEAR		0x1583
#define PA_RXTERMINATION	0x1584

#define HCS_DP		(1u << 0)
#define HCS_UTRLRDY	(1u << 1)
#define HCS_UCRDY	(1u << 3)

#define BLOCK		4096u
#define PRDT_MAX	4096u	/* bytes per PRDT entry on Exynos */
#define NPRDT		8	/* up to 32 KiB per command */
#define CMD_MAX		(NPRDT * PRDT_MAX)
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
#ifdef UFSPROBE
static uint8_t data[CMD_MAX] __attribute__((aligned(4096)));
#endif
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

#ifdef UFSPROBE
static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
#endif

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

#ifdef UFSPROBE
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
#endif

/*
 * One UIC command. Returns the config result code (ARG2[7:0], 0 = success)
 * or 0x100 on timeout; arg3 is DME_SET's value, *val gets ARG3 back (DME_GET's
 * value). IS bits it raised are returned in *is.
 */
static uint32_t uic_cmd(uint32_t op, uint32_t arg1, uint32_t arg3, uint32_t *val, uint32_t *is)
{
	uint64_t start = ticks();
	while (!(ufs_rd(REG_HCS) & HCS_UCRDY))
		if (ticks() - start > TICKS_1S)
			return 0x101;
	ufs_wr(REG_IS, IS_UCCS | IS_UHXS | IS_UHES | IS_UPMS);
	ufs_wr(REG_UCMDARG1, arg1);
	ufs_wr(REG_UCMDARG2, 0);
	ufs_wr(REG_UCMDARG3, arg3);
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
	uint32_t rc = uic_cmd(UIC_DME_GET, attr << 16, 0, &val, &is);
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

#define UNIPRO_BASE		0x13280000UL
#define PMA_BASE		0x13204000UL
#define CPORT_BASE		0x13208000UL
#define UNIP_PA_DBG_OPTION_SUITE_1	0x39A8
#define DBG_SUITE1_ENABLE		0x90913C1C
#define UNIP_PA_DBG_OPTION_SUITE_2	0x39B4
#define DBG_SUITE2_ENABLE		0xE01C115F
#define UNIP_PA_AVAILTXDATALENS		0x3080
#define UNIP_PA_AVAILRXDATALENS		0x3100
#define VS_UFSHCI_V2P1_CTRL		0x8C	/* bit16 IA_TICK_SEL */
#define VS_ERROR_EN_DL		0x7C
#define VS_ERROR_EN_N		0x80
#define VS_ERROR_EN_T		0x84
#define PA_CONNECTEDRXLANES	0x1581
#define PA_MAXRXHSGEAR		0x1587
/* Fallback only: ABL's HCI_1US_TO_CNT_VAL (0xb2). The real rate is read back
 * from ABL's UniPro programming (see abl_mclk_rate). */
#define UFS_MCLK_RATE_GUESS	178000000u
#define UNIP_COMP_CLK_PERIOD	0x44	/* CAL UNIPRO_DBG_PRD: 16e9 / mclk_rate */
static uint32_t ufs_mclk_rate = UFS_MCLK_RATE_GUESS;

/* ABL ran the same CAL: recover its mclk from the period it programmed. */
static void abl_mclk_rate(void)
{
	uint32_t prd = *(volatile uint32_t *)(0x13280000UL + UNIP_COMP_CLK_PERIOD);
	if (prd)
		ufs_mclk_rate = (uint32_t)(16000000000ULL / prd);
	con_hex2("UFS MCLK", "abl_prd=", prd, "rate=", ufs_mclk_rate);
}

static struct ufs_vs_handle cal_handle;
static struct ufs_cal_param cal_param;
static struct uic_pwr_mode cal_pmd;

static void cal_udelay(u32 us)
{
	delay_us(us);
}

#define FORCE_HCS_UNIPRO_MCLK_STOP_EN	(1u << 5)
#define FORCE_HCS_MPHY_APBCLK_STOP_EN	(1u << 10)

/*
 * Linux's ufs_call_cal: the M-PHY's APB clock must run while the CAL writes
 * PMA registers (else the writes are dropped and it still reports success),
 * then may stop again.
 */
static uint32_t cal_call(uint32_t (*fn)(struct ufs_cal_param *))
{
	vs_wr(VS_FORCE_HCS, vs_rd(VS_FORCE_HCS) & ~(FORCE_HCS_UNIPRO_MCLK_STOP_EN | FORCE_HCS_MPHY_APBCLK_STOP_EN));
	uint32_t ret = fn(&cal_param);
	vs_wr(VS_FORCE_HCS, vs_rd(VS_FORCE_HCS) | FORCE_HCS_MPHY_APBCLK_STOP_EN);
	return ret;
}


static uint32_t dme_get(uint32_t attr)
{
	uint32_t val = 0, is = 0;
	return uic_cmd(UIC_DME_GET, attr << 16, 0, &val, &is) ? 0xffffffffu : val;
}

/*
 * Bring the host and link up from scratch the way Linux's ufshcd + ufs-exynos
 * do (ufshcd_hba_execute_hce / ufshcd_link_startup and the exynos notify
 * hooks), with Samsung's CAL providing the PHY/UniPro configuration around
 * link startup; then run the UFS device init (NOP OUT, fDeviceInit). Returns
 * 0 when the device is ready for SCSI commands from our list.
 */
static int host_reset_and_link(void)
{
	con_str("UFS INIT", "host + link bring-up with the zuma CAL");
	cal_handle.std = (void *)UFS_BASE;
	cal_handle.hci = (void *)VS_BASE;
	cal_handle.ufsp = 0;			/* secure-only: the CAL must not need it */
	cal_handle.unipro = (void *)UNIPRO_BASE;
	cal_handle.pma = (void *)PMA_BASE;
	cal_handle.cport = (void *)CPORT_BASE;
	cal_handle.udelay = cal_udelay;
	cal_pmd.lane = 2;			/* DT ufs,pmd-attr-lane / -gear */
	cal_pmd.gear = 4;
	cal_pmd.mode = 1;			/* FAST */
	cal_pmd.hs_series = 2;			/* rate B */
	cal_param.handle = &cal_handle;
	cal_param.mclk_rate = ufs_mclk_rate;
	cal_param.tbl = HOST_EMBD;
	cal_param.board = BRD_SMDK;		/* DT brd-for-cal = 1 */
	cal_param.evt_ver = 0;			/* no evt-ver in the DT */
	cal_param.pmd = &cal_pmd;
	ufs_cal_init(&cal_param, 0);

	/*
	 * Reset the device over the link ABL left up, before tearing the host
	 * down: if HCI_GPIO_OUT does not drive its RST_n, the device would keep
	 * its old link session and refuse a fresh link startup.
	 */
	con_hex("UFS INIT gpio_out", vs_rd(VS_GPIO_OUT));
	/*
	 * Pin mux (zuma-base.dts pinctrl@13060000, sole bank gph5; Samsung
	 * layout CON +0 [4 bits/pin], DAT +4, PUD +8, DRV +0xc): gph5-0 is the
	 * device REF_CLK out and gph5-1 its RST_n, both function 2 (controller
	 * driven). ABL left the device in reset; if it also took RST_n over as a
	 * plain GPIO, HCI_GPIO_OUT never reaches the pin. Linux's pinctrl sets
	 * function 2 at UFS probe.
	 */
	/*
	 * Device VCC: DT "ufs-vcc" is a regulator-fixed, enable-active-high, on
	 * gpp0-1 (pinctrl@10840000, gpp0 is its first bank). ABL powers the
	 * device down with the rest of UFS; turn it back on (only pin 1 touched).
	 */
	{
		volatile uint32_t *gpp0 = (volatile uint32_t *)0x10840000UL;
		con_hex2("UFS GPP0", "con=", gpp0[0], "dat=", gpp0[1]);
		uint32_t con = gpp0[0], dat = gpp0[1];
		if (((con >> 4) & 0xf) != 1 || !(dat & 2)) {
			gpp0[1] = dat | 2;			/* drive high first */
			gpp0[0] = (con & ~0xf0u) | 0x10u;	/* pin1 -> output */
			delay_us(20000);			/* VCC ramp */
			con_hex2("UFS GPP0", "con'=", gpp0[0], "dat'=", gpp0[1]);
		}
	}
	{
		volatile uint32_t *gph5 = (volatile uint32_t *)0x13060000UL;
		con_hex2("UFS GPH5", "con=", gph5[0], "dat=", gph5[1]);
		con_hex2("UFS GPH5", "pud=", gph5[2], "drv=", gph5[3]);
		uint32_t con = gph5[0];
		uint32_t want = (con & ~0xffu) | 0x22u;	/* pin0, pin1 -> function 2 */
		if (con != want) {
			gph5[0] = want;
			con_hex2("UFS GPH5", "con'=", gph5[0], "dat'=", gph5[1]);
		}
	}
	{
		uint32_t is = 0;
		uint32_t rc0 = uic_cmd(UIC_DME_ENDPOINTRESET, 0, 0, 0, &is);
		con_hex2("UFS EPRESET", "rc=", rc0, "is=", is);
		delay_us(50000);
	}

	/* ufshcd_hba_stop */
	ufs_wr(REG_HCE, 0);
	uint64_t start = ticks();
	while (ufs_rd(REG_HCE) & 1)
		if (ticks() - start > TICKS_1S / 10) {
			con_str("UFS INIT", "HCE stuck at 1");
			return 1;
		}

	/* hce PRE: exynos_ufs_init_host (sw reset + config_host), device reset */
	vs_wr(VS_SW_RST, 3);
	start = ticks();
	while (vs_rd(VS_SW_RST) & 3)
		if (ticks() - start > TICKS_1S / 100) {
			con_str("UFS INIT", "sw reset stuck");
			return 2;
		}
	vs_wr(VS_FORCE_HCS, vs_rd(VS_FORCE_HCS) & ~FORCE_HCS_CORECLK_STOP_EN);
	vs_wr(VS_UFSHCI_V2P1_CTRL, vs_rd(VS_UFSHCI_V2P1_CTRL) | (1u << 16));
	vs_wr(VS_1US_TO_CNT_VAL, (ufs_mclk_rate / 1000000u) & 0x3ff);
	vs_wr(VS_DATA_REORDER, 0xA);
	vs_wr(VS_TXPRDT_ENTRY_SIZE, (1u << 31) | 12);
	vs_wr(VS_RXPRDT_ENTRY_SIZE, 12);
	vs_wr(VS_UTRL_NEXUS_TYPE, ~0u);
	vs_wr(VS_UTMRL_NEXUS_TYPE, ~0u);
	vs_wr(VS_AXIDMA_BURST, (1u << 31) | (3u << 27) | 3u);	/* WLU_EN | BURST_LEN(3) */
	vs_wr(VS_IOP_ACG_DISABLE, vs_rd(VS_IOP_ACG_DISABLE) & ~1u);
	*(volatile uint32_t *)(UNIPRO_BASE + UNIP_PA_DBG_OPTION_SUITE_1) = DBG_SUITE1_ENABLE;
	*(volatile uint32_t *)(UNIPRO_BASE + UNIP_PA_DBG_OPTION_SUITE_2) = DBG_SUITE2_ENABLE;
	vs_wr(VS_GPIO_OUT, 0);			/* device RST_n pulse */
	delay_us(5);
	vs_wr(VS_GPIO_OUT, 1);
	delay_us(50000);			/* let the device come out of reset */

	/* ufshcd_hba_start */
	ufs_wr(REG_HCE, 1);
	start = ticks();
	while (!(ufs_rd(REG_HCE) & 1))
		if (ticks() - start > TICKS_1S / 10) {
			con_str("UFS INIT", "HCE never set");
			return 3;
		}
	/* hce POST: clock-stop control on, clocks ungated, available lanes */
	vs_wr(VS_FORCE_HCS, vs_rd(VS_FORCE_HCS) | FORCE_HCS_CLK_STOP_CTRL_ALL);
	vs_wr(VS_CLKSTOP_CTRL, vs_rd(VS_CLKSTOP_CTRL) & ~0x17u);
	uint32_t avail_tx = *(volatile uint32_t *)(UNIPRO_BASE + UNIP_PA_AVAILTXDATALENS);
	uint32_t avail_rx = *(volatile uint32_t *)(UNIPRO_BASE + UNIP_PA_AVAILRXDATALENS);
	con_hex2("UFS INIT", "avail_tx=", avail_tx, "avail_rx=", avail_rx);
	cal_param.available_lane = (uint8_t)(avail_rx ? avail_rx : 2);

	/* link startup PRE: error reporting, unipro options, CAL pre_link */
	vs_wr(VS_ERROR_EN_DL, (1u << 31) | 0x20 | 0x2000);
	vs_wr(VS_ERROR_EN_N, (1u << 31) | 0x1 | 0x2 | 0x4);
	vs_wr(VS_ERROR_EN_T, (1u << 31) | 0x1 | 0x2 | 0x4 | 0x10);
	*(volatile uint32_t *)(UNIPRO_BASE + UNIP_PA_DBG_OPTION_SUITE_1) = DBG_SUITE1_ENABLE;
	*(volatile uint32_t *)(UNIPRO_BASE + UNIP_PA_DBG_OPTION_SUITE_2) = DBG_SUITE2_ENABLE;
	uint32_t cal = cal_call((uint32_t (*)(struct ufs_cal_param *))ufs_cal_pre_link);
	con_hex("UFS CAL pre_link", cal);
	if (cal)
		return 4;

	uint32_t rc = 0x100, is = 0;
	for (int i = 0; i < 3 && rc; i++) {
		rc = uic_cmd(UIC_DME_LINKSTARTUP, 0, 0, 0, &is);
		con_hex2("UFS LINKUP", "rc=", rc, "is=", is);
		con_hex2("UFS LINKUP", "hcs=", ufs_rd(REG_HCS), "dp=", ufs_rd(REG_HCS) & HCS_DP);
		/* UIC error codes: which layer failed (UFSHCI UECPA..UECDME). */
		con_hex2("UFS UEC", "pa=", ufs_rd(0x60), "dl=", ufs_rd(0x64));
		con_hex2("UFS UEC", "n=", ufs_rd(0x68), "t=", ufs_rd(0x6c));
		con_hex2("UFS UEC", "dme=", ufs_rd(0x70), "upmcrs=", (ufs_rd(REG_HCS) >> 8) & 7);
		/* UniPro's own DME confirm results (ufs-vs-regs.h UNIP_DME_*_CNF_RESULT). */
		volatile uint32_t *up = (volatile uint32_t *)UNIPRO_BASE;
		con_hex2("UFS UNIP", "ver=", up[0x000 / 4], "info=", up[0x004 / 4]);
		con_hex2("UFS UNIP", "poweron=", up[0x7804 / 4], "enable=", up[0x7834 / 4]);
		con_hex2("UFS UNIP", "linkstart=", up[0x7854 / 4], "reset_lvl=", up[0x7824 / 4]);
		con_hex2("UFS UNIP", "conn_tx=", up[0x3184 / 4], "conn_rx=", up[0x3204 / 4]);
		if (!rc && !(ufs_rd(REG_HCS) & HCS_DP))
			rc = 0x200;
	}
	if (rc)
		return 5;

	/* link startup POST: capabilities after link, CAL post_link */
	cal_param.connected_tx_lane = (uint8_t)dme_get(PA_CONNECTEDTXLANES);
	cal_param.connected_rx_lane = (uint8_t)dme_get(PA_CONNECTEDRXLANES);
	uint32_t max_rx_gear = dme_get(PA_MAXRXHSGEAR);
	cal_param.max_gear = (uint8_t)(max_rx_gear < cal_pmd.gear ? max_rx_gear : cal_pmd.gear);
	con_hex2("UFS CAPS", "lanes tx/rx=", cal_param.connected_tx_lane << 8 | cal_param.connected_rx_lane,
	    "max_gear=", cal_param.max_gear);
	cal = cal_call((uint32_t (*)(struct ufs_cal_param *))ufs_cal_post_link);
	con_hex("UFS CAL post_link", cal);
	if (cal)
		return 6;
	dme_get_log("UFS PWRMODE", PA_PWRMODE);
	ufs_wr(REG_IS, ufs_rd(REG_IS));

	/* make operational: our list, running */
	ufs_wr(REG_UTRLBA, (uint32_t)(uintptr_t)utrl);
	ufs_wr(REG_UTRLBAU, 0);
	ufs_wr(REG_UTRLRSR, 1);
	con_hex2("UFS INIT", "utrlba=", ufs_rd(REG_UTRLBA), "rsr=", ufs_rd(REG_UTRLRSR));
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
		return 7;
	int f = query_flag(0x06, 0x01);		/* set fDeviceInit */
	con_hex("UFS fDevInit set", (uint32_t)f);
	for (int i = 0; i < 200 && f != 0; i++) {
		delay_us(5000);
		f = query_flag(0x05, 0x01);
	}
	con_hex("UFS fDevInit", (uint32_t)f);
	return f == 0 ? 0 : 8;
}

/*
 * UFS PHY isolation (DT ufs-phy-iso: PMU offset 0x3ec0, bit0 = 1 releases).
 * PMU_ALIVE reads are plain MMIO; writes go through EL3's secure-register
 * SMC, as Linux's exynos-pmu-if does (set_priv_reg).
 */
#define PMU_ALIVE		0x15460000UL
#define PMU_UFS_PHY_ISO		0x3ec0
#define SMC_CMD_PRIV_REG	0x82000504u
#define PRIV_REG_OPTION_WRITE	1u

static uint64_t smc_priv_write(uint64_t addr, uint32_t val)
{
	register uint64_t x0 __asm__("x0") = SMC_CMD_PRIV_REG;
	register uint64_t x1 __asm__("x1") = addr;
	register uint64_t x2 __asm__("x2") = PRIV_REG_OPTION_WRITE;
	register uint64_t x3 __asm__("x3") = val;
	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
	    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
	    "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static void phy_iso_release(void)
{
	volatile uint32_t *iso = (volatile uint32_t *)(PMU_ALIVE + PMU_UFS_PHY_ISO);
	uint32_t v = *iso;
	con_hex("UFS PHYISO", v);
	if (!(v & 1)) {
		uint64_t r = smc_priv_write(PMU_ALIVE + PMU_UFS_PHY_ISO, v | 1);
		delay_us(100);
		con_hex2("UFS PHYISO", "smc=", (uint32_t)r, "now=", *iso);
	}
}

static uint32_t dme_set(uint32_t attr, uint32_t val)
{
	uint32_t is = 0;
	return uic_cmd(UIC_DME_SET, attr << 16, val, 0, &is);
}

/*
 * Power mode change to HS-G4 rate B, both lanes, FAST (ufshcd_change_power_mode
 * with exynos_ufs_pwr_change_notify around it). The Exynos host has
 * UFSHCD_QUIRK_SKIP_DEF_UNIPRO_TIMEOUT_SETTING: no PA_PWRMODEUSERDATA writes.
 * Returns 0 once the host reports the local change done (UPMCRS = PWR_LOCAL).
 */
static int ufs_hs_mode(void)
{
	uint8_t lanes = cal_param.connected_rx_lane, gear = cal_param.max_gear;
	if (!lanes || lanes != cal_param.connected_tx_lane || gear < 1)
		return 1;
	cal_pmd.lane = lanes;
	cal_pmd.gear = gear;
	uint32_t cal = cal_call((uint32_t (*)(struct ufs_cal_param *))ufs_cal_pre_pmc);
	con_hex("UFS CAL pre_pmc", cal);
	if (cal)
		return 2;
	uint32_t err = dme_set(PA_RXGEAR, gear) | dme_set(PA_ACTIVERXDATALANES, lanes) |
	    dme_set(PA_RXTERMINATION, 1) | dme_set(PA_TXGEAR, gear) |
	    dme_set(PA_ACTIVETXDATALANES, lanes) | dme_set(PA_TXTERMINATION, 1) |
	    dme_set(PA_HSSERIES, cal_pmd.hs_series);
	if (err) {
		con_hex("UFS PMC set", err);
		return 3;
	}
	/* PA_PWRMODE = FAST rx << 4 | FAST tx; completion is IS.UPMS. */
	uint32_t is = 0;
	uint32_t rc = uic_cmd(UIC_DME_SET, PA_PWRMODE << 16, 0x11, 0, &is);
	uint64_t start = ticks();
	while (!rc && !(is & IS_UPMS)) {
		is |= ufs_rd(REG_IS);
		if (ticks() - start > TICKS_1S) {
			rc = 0x100;
			break;
		}
	}
	ufs_wr(REG_IS, IS_UPMS);
	uint32_t upmcrs = (ufs_rd(REG_HCS) >> 8) & 7;
	con_hex2("UFS PMC", "rc=", rc, "upmcrs=", upmcrs);
	if (rc || upmcrs != 1)
		return 4;
	cal_param.active_tx_lane = (uint8_t)*(volatile uint32_t *)(UNIPRO_BASE + 0x3180);
	cal_param.active_rx_lane = (uint8_t)*(volatile uint32_t *)(UNIPRO_BASE + 0x3200);
	cal = cal_call((uint32_t (*)(struct ufs_cal_param *))ufs_cal_post_pmc);
	con_hex("UFS CAL post_pmc", cal);
	dme_get_log("UFS PWRMODE", PA_PWRMODE);
	dme_get_log("UFS TXGEAR", PA_TXGEAR);
	return cal ? 5 : 0;
}

/* TUR until the device stops reporting a unit attention; 0 when ready. */
static uint32_t ufs_tur(void)
{
	static const uint8_t tur[6] = { 0x00 };
	uint32_t ocs = 0, st = 0, rc = 1;
	for (int i = 0; i < 5; i++) {
		rc = scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st);
		con_hex2("UFS TUR", "rc=", rc, "ocs/st=", ocs << 8 | st);
		if (!rc)
			break;
	}
	return rc == 4 ? 0 : rc;	/* a CHECK CONDITION still means it answers */
}

#ifdef UFSPROBE
/* Capacity, the big GPT partitions, and a read throughput sample. */
static void ufs_report(void)
{
	uint32_t ocs = 0, st = 0, rc;
	static const uint8_t cap10[10] = { 0x25 };	/* READ CAPACITY(10) */
	rc = scsi(0, cap10, sizeof(cap10), data, 8, &ocs, &st);
	if (!rc)
		con_hex2("UFS LUN0", "lastlba=", be32(data), "blk=", be32(data + 4));
	if (read_blocks(1, 1, data))
		return;
	uint32_t nent = data[80] | data[81] << 8 | data[82] << 16, esz = data[84] | data[85] << 8;
	uint32_t elba = data[72] | data[73] << 8;
	con_hex2("UFS GPT", "entries=", nent, "lba=", elba);
	if (esz == 128 && nent <= 128)
		list_partitions(elba, nent);
	/* 16 MiB from the start of LUN0 in 32 KiB commands. */
	uint64_t t0 = ticks();
	for (uint32_t lba = 0; lba < 4096; lba += NPRDT)
		if (read_blocks(lba, NPRDT, data)) {
			con_str("UFS SPEED", "read error");
			return;
		}
	uint64_t us = (ticks() - t0) / (TICKS_1S / 1000000u);
	con_hex2("UFS SPEED", "16MiB_us=", (uint32_t)us, "KiB/s=", (uint32_t)(16384ULL * 1000000u / (us ? us : 1)));
}
#endif

int ufs_ready;

/*
 * ABL powers the UFS down before handing over; bring it back up the way
 * Linux does and leave it operational for xnu (bsd/dev/husky_ufs.c), which
 * takes over the transfer request list: host enabled, link in HS-G4 (PWM if
 * the power mode change fails), device initialised, auto-hibern8 off,
 * no request outstanding. Read-only: no SCSI WRITE exists in this file.
 */
void ufs_bringup(void)
{
	con_str("UFS", "bring-up v18: CAL link + HS-G4 for xnu");
	phy_iso_release();
	abl_mclk_rate();
	/* Requests held or host clocks stopped would leave a doorbell pending forever. */
	vs_wr(VS_REQ_HOLD_EN, 0);
	vs_wr(VS_CLKSTOP_CTRL, vs_rd(VS_CLKSTOP_CTRL) & ~0x1fu);
	ufs_wr(REG_IS, ufs_rd(REG_IS));
	int r = host_reset_and_link();
	con_hex("UFS LINK result", (uint32_t)r);
	if (r)
		return;
	r = ufs_hs_mode();
	con_hex("UFS HS result", (uint32_t)r);
	if (r) {
		/* A failed power mode change can leave the link unusable: start over in PWM. */
		r = host_reset_and_link();
		con_hex("UFS PWM relink", (uint32_t)r);
		if (r)
			return;
	}
	ufs_wr(REG_AHIT, 0);
	if (ufs_tur())
		return;
#ifdef UFSPROBE
	ufs_report();
#endif
	ufs_wr(REG_IS, ufs_rd(REG_IS));
	ufs_ready = 1;
	con_str("UFS", "ready for xnu");
}
