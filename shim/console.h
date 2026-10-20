#pragma once
#include <stdint.h>

/*
 * The log goes to the ramoops console zone, which Android shows on the
 * next boot as /sys/fs/pstore/console-ramoops-0. Stock ramoops is
 * 0x400000@0xfd3ff000 with record_size=0, console_size=2M, ecc=0, so the
 * console zone is the first 2M of the region (fs/pstore/ram.c lays out
 * dmesg, console, ftrace, pmsg in that order).
 */
#define RAMOOPS_CON_BASE	0xfd3ff000UL
#define RAMOOPS_CON_SIZE	0x200000UL

/* Colours are kept so sweet-derived callers build; the text log ignores them. */
#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U

/* Starts a fresh log, dropping whatever the zone held. */
void con_clear(uint32_t bg);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
