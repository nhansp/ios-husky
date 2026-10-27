#pragma once
#include <IOKit/IOKitLib.h>
typedef struct __IOSurface *IOSurfaceRef;
#define kIOSurfaceLockReadOnly 1
uint32_t IOSurfaceGetID(IOSurfaceRef); size_t IOSurfaceGetWidth(IOSurfaceRef), IOSurfaceGetHeight(IOSurfaceRef), IOSurfaceGetBytesPerRow(IOSurfaceRef);
size_t IOSurfaceGetPlaneCount(IOSurfaceRef), IOSurfaceGetAllocSize(IOSurfaceRef), IOSurfaceGetBytesPerElement(IOSurfaceRef);
size_t IOSurfaceGetBytesPerRowOfPlane(IOSurfaceRef, size_t), IOSurfaceGetBytesPerElementOfPlane(IOSurfaceRef, size_t), IOSurfaceGetWidthOfPlane(IOSurfaceRef, size_t), IOSurfaceGetHeightOfPlane(IOSurfaceRef, size_t);
void *IOSurfaceGetBaseAddress(IOSurfaceRef); void *IOSurfaceGetBaseAddressOfPlane(IOSurfaceRef, size_t);
IOReturn IOSurfaceLock(IOSurfaceRef, uint32_t, uint32_t *); IOReturn IOSurfaceUnlock(IOSurfaceRef, uint32_t, uint32_t *);
uint32_t IOSurfaceGetSeed(IOSurfaceRef);
