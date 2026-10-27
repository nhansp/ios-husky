#pragma once
#ifdef __OBJC__
#else
typedef void *id;
#endif
#include <stdint.h>
#include <stddef.h>
typedef long dispatch_once_t;
typedef void *dispatch_queue_t; typedef id dispatch_data_t;
#define DISPATCH_DATA_DESTRUCTOR_DEFAULT ((void*)0)
#define QOS_CLASS_USER_INTERACTIVE 0x21
void dispatch_once(dispatch_once_t *p, void (^b)(void));
void dispatch_async(dispatch_queue_t q, void (^b)(void));
dispatch_queue_t dispatch_get_global_queue(long id, unsigned long flags);
dispatch_data_t dispatch_data_create(const void *b, size_t s, dispatch_queue_t q, void *d);
size_t dispatch_data_get_size(dispatch_data_t d);
dispatch_data_t dispatch_data_create_map(dispatch_data_t d, const void **b, size_t *s);
