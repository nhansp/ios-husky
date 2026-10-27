#pragma once
typedef void *os_log_t;
os_log_t os_log_create(const char *, const char *);
#define os_log(l, ...) ((void)(l), (void)sizeof(__VA_ARGS__))
#define os_log_error(l, ...) ((void)(l), (void)sizeof(__VA_ARGS__))
