#pragma once
struct mach_header_64;
const unsigned char *getsectiondata(const struct mach_header_64 *, const char *, const char *, unsigned long *);
