#pragma once
#include <stdint.h>
typedef uint32_t io_service_t, io_object_t;
typedef const void *CFTypeRef, *CFDataRef, *CFStringRef;
typedef void *CFAllocatorRef;
typedef long CFIndex; typedef unsigned long CFTypeID;
extern CFAllocatorRef kCFAllocatorDefault;
#define CFSTR(x) ((CFStringRef)(x))
CFTypeRef IORegistryEntryCreateCFProperty(io_service_t e, CFStringRef k, CFAllocatorRef a, uint32_t o);
CFTypeID CFGetTypeID(CFTypeRef); CFTypeID CFDataGetTypeID(void);
const uint8_t *CFDataGetBytePtr(CFDataRef); CFIndex CFDataGetLength(CFDataRef);
void CFRelease(CFTypeRef); CFTypeRef CFRetain(CFTypeRef);
typedef uint32_t kern_return_t, IOReturn;
#define kIOReturnSuccess 0
