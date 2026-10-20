#pragma once
#include <stdint.h>

/* Boot the HUSKYXNU payload found in the ABL ramdisk; returns only on error
 * (the returned string names the failure). dtb is the ABL FDT pointer. */
const char *loader_boot(uintptr_t dtb);
