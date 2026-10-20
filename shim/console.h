#pragma once
#include <stdint.h>

/*
 * The log goes to the ramoops *pmsg* zone, which Android shows on the next
 * boot as /sys/fs/pstore/pmsg-ramoops-0. Stock ramoops is 0x400000@0xfd3ff000
 * with record_size=0, console_size=2M, ftrace=0, pmsg_size=2M, ecc=0, so
 * fs/pstore/ram.c lays the zones out as dmesg(empty), console@0xfd3ff000+2M,
 * ftrace(empty), pmsg@0xfd5ff000+2M. We deliberately avoid the console zone:
 * ABL rewrites it with its own boot log every boot (including the one that
 * boots Android after our reset), which would clobber our log before it can
 * be read. ABL leaves the pmsg zone untouched, so our log survives the
 * handoff. Both zones share the persistent_ram_buffer sig 0x43474244.
 */
#define RAMOOPS_CON_BASE	0xfd5ff000UL
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
