#pragma once
#include <stdint.h>
#define CC_SHA256_DIGEST_LENGTH 32
typedef uint32_t CC_LONG;
unsigned char *CC_SHA256(const void *d, CC_LONG l, unsigned char *md);
