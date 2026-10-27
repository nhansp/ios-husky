#pragma once
#include <stddef.h>
typedef struct objc_ivar *Ivar;
#define Nil ((Class)0)
#define nil ((id)0)
Ivar class_getInstanceVariable(Class c, const char *n);
const char *ivar_getTypeEncoding(Ivar v);
ptrdiff_t ivar_getOffset(Ivar v);
size_t class_getInstanceSize(Class c);
