#pragma once
#include <stdint.h>
void test_fb(void);
int fb_handoff(uint64_t *base, uint32_t *width, uint32_t *height, uint32_t *pitch);
