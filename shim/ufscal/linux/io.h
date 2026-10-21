/* MMU off: all MMIO is Device memory, 32-bit volatile accesses. */
#pragma once
#include <stdint.h>
static inline void writel(uint32_t v, volatile void *a) { *(volatile uint32_t *)a = v; }
static inline uint32_t readl(const volatile void *a) { return *(const volatile uint32_t *)a; }
