/*
 * Text log in the ramoops console zone (see console.h). The zone is a
 * struct persistent_ram_buffer from fs/pstore/ram_core.c: sig, start
 * (write offset), size (valid bytes), then data. Linux keeps the old
 * contents when sig matches and start <= size <= capacity. We never wrap,
 * so start == size, and both are updated after every line so a hang still
 * leaves everything logged so far. With the MMU off every access is
 * Device memory, so the data is written a byte at a time.
 */
#include "console.h"

#define PERSISTENT_RAM_SIG	0x43474244U	/* "DBGC"; console zone sig is 0 ^ this */
#define LABEL_COLS		8

struct prz_buffer {
	uint32_t sig;
	uint32_t start;
	uint32_t size;
	uint8_t data[];
};

#define PRZ		((volatile struct prz_buffer *)RAMOOPS_CON_BASE)
#define PRZ_CAP		(RAMOOPS_CON_SIZE - sizeof(struct prz_buffer))

static uint32_t pos;

static void put_char(char ch)
{
	if (pos < PRZ_CAP)
		PRZ->data[pos++] = ch;
}

static void put_str(const char *s)
{
	while (*s)
		put_char(*s++);
}

static void end_line(void)
{
	put_char('\n');
	__asm__ volatile("dsb sy" ::: "memory");
	PRZ->start = pos;
	PRZ->size = pos;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void put_label(const char *label)
{
	int col = 0;
	while (*label) {
		put_char(*label++);
		col++;
	}
	do
		put_char(' ');
	while (++col < LABEL_COLS);
}

static void fmt_hex(char *buf, uint64_t v, int digits)
{
	static const char hex[] = "0123456789abcdef";
	for (int i = 0; i < digits; i++)
		buf[i] = hex[(v >> (4 * (digits - 1 - i))) & 0xf];
	buf[digits] = 0;
}

void con_clear(uint32_t bg)
{
	(void)bg;
	/*
	 * ABL writes its own log here through a cacheable mapping just before
	 * jumping to us; clean+invalidate the zone so none of its dirty lines
	 * can be evicted over our Device-memory writes later.
	 */
	for (uintptr_t a = RAMOOPS_CON_BASE; a < RAMOOPS_CON_BASE + RAMOOPS_CON_SIZE; a += 64)
		__asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
	pos = 0;
	PRZ->sig = PERSISTENT_RAM_SIG;
	PRZ->start = 0;
	PRZ->size = 0;
	__asm__ volatile("dsb sy" ::: "memory");
}

void con_skip(void)
{
	end_line();
}

void con_str(const char *label, const char *s)
{
	put_label(label);
	put_str(s);
	end_line();
}

void con_hex(const char *label, uint64_t v)
{
	char buf[17];
	fmt_hex(buf, v, 16);
	con_str(label, buf);
}

void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	char buf[9];
	put_label(label);
	put_str(a);
	fmt_hex(buf, va, 8);
	put_str(buf);
	if (b) {
		put_char(' ');
		put_str(b);
		fmt_hex(buf, vb, 8);
		put_str(buf);
	}
	end_line();
}
