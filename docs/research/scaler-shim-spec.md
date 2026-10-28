# AppleM2Scaler user-client ABI (iOS 26.5 / 23F77) and shim plan

Sources (all read-only analysis of extracted binaries):
- DSC `artifacts/research/dsc-26.5/cache/dyld_shared_cache_arm64e`. Addresses below are unslid DSC vmaddrs.
  - IOSurfaceAccelerator image = `/System/Library/PrivateFrameworks/IOSurfaceAccelerator.framework/IOSurfaceAccelerator` ("IOSA").
  - QuartzCore image ("CA") for the real backboardd caller.
- Kernelcache `ipsw-26.5/kernelcache.release.iPhone12,3_5`, kext `com.apple.driver.AppleM2ScalerCSCDriver` (265.0.0, listed at 0xfffffff007487d30). The kernelcache has 0 defined symbols; function names below come from log/format strings inside the kext, and addresses are unslid kernelcache vmaddrs.
- Tag INFERRED = not directly read from code, deduced from structure.

Key conclusions up front:
1. The userland framework talks to the kernel with **one struct method per transfer** (selector 1, 0x1b0 byte input, no output). There are no scalar-in selectors on the hot path and no IOSurface mach ports. Surfaces are identified by the 32-bit IOSurface ID.
2. Pixel formats, plane layout, and YCbCr range are NOT in the struct. The kernel service must derive them from the two IOSurfaces it looks up by ID (HuskySurface already stores pixelFormat, planes[], bytesPerRow).
3. The kernelcache only contains `AppleM2ScalerCSCDriver`; there is no `AppleM2ScalerParavirtDriver` in the iPhone12,3_5 kernelcache (strings search of the kext returned nothing; ipsw kernel kexts lists only the CSC kext). Shim the CSC name.
4. CoreAnimation (backboardd) uses the **asynchronous** form: it passes a completion callback, so the shim must deliver an async completion message, otherwise CA never finishes the transfer.

---------------------------------------------------------------------------

## 1. How userland finds and opens the service

### 1.1 IOSurfaceAcceleratorCreate (IOSA 0x25cafd858)
Signature as used: `IOSurfaceAcceleratorCreate(CFAllocatorRef, CFDictionaryRef props, IOSurfaceAcceleratorRef *out)`. x1 = props, x2 = out (stored in x20/x23 at 0x25cafd878/0x25cafd87c).

Flow (all addresses in IOSA):
1. Object is a 0x20 byte CFRuntime instance. `+0x10` = IONotificationPort, `+0x18` = runloop source, `+0x20` = io_service_t, `+0x24` = io_connect_t, `+0x2c` = SRS-mode flag (0 for the scaler).
2. Service name choice:
   - If props contains key `Sharpener` (CFString at 0x28625b118) the service is `AppleSRSDriver` (string 0x25cb0067d) and the IOServiceOpen type = `strength | 0xb00000` (0x25cafd91c). Not relevant to the scaler.
   - Otherwise service name = `"AppleM2ScalerCSCDriver"` (string 0x25cb01666) and IOServiceOpen type = 0 (`mov w22,#0` at 0x25cafd92c).
3. `checkIsVirtualized` (block at 0x25cafea58, once-guarded):
   - `sysctlbyname("kern.hv_vmm_present")` (string 0x25cb019e7). If the sysctl fails or returns 0, `is_virtualized = 0` (0x25cafeaa8 and the `b.eq` at 0x25cafeaac).
   - If it returns nonzero it additionally reads `sysctlbyname("debug.iosa.simulated_model")` (0x25cb019fb); `is_virtualized` becomes 1 only if that sysctl FAILS or returns 0 (cset eq at 0x25cafeae4).
   - Our xnu: `kern.hv_vmm_present` on arm is `IODTGetDefault("vmm-present")` (bsd/kern/hvg_sysctl.c:143). With no such DT property it is 0, so IOSA takes the CSC branch.
4. Matching (no fallback inside IOSA):
   - not virtualized: `IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleM2ScalerCSCDriver"))` at 0x25cafd9f0..0x25cafda00. On failure prints `"IOServiceMatchingfailed for: %s\n"` (0x25cb01703, via fprintf stderr) and returns `0xe00002c0` (kIOReturnNotFound-class `mov w22,#0x2c0; movk #0xe000`). This is the backboardd log line.
   - virtualized: `IOServiceMatching("AppleM2ScalerParavirtDriver")` (string 0x25cb016ad); failure prints the fixed string `"IOServiceMatchingfailed for: AppleM2ScalerParavirtDriver\n"` (0x25cb016c9) and returns 0xe00002c0.
   - So IOSA never tries both. The two-step CSC-then-Paravirt matching the user saw comes from `CA::ScalerUtil::get_io_service` (QuartzCore block 0x18400a728): CSC first (string 0x18425b46d), then `"AppleM2ScalerParavirtDriver"` (0x18425b484) if CSC returns 0; result stored in a static (`scaler_service`) that only feeds registry property reads (section 1.3).
5. `IOServiceOpen(service, mach_task_self(), type, &conn)` at 0x25cafda1c. Failure -> object released and the kern_return_t returned. Success: `[obj+0x20]=service`, `[obj+0x24]=conn`, `*out = obj`.
6. When not virtualized, one struct call follows at create time (0x25cafda60): `IOConnectCallStructMethod(conn, 0xa, &prop, 0x18, NULL, NULL)`. This is "set client property", see 2.4. Failure only prints `"Failed to set client property for: %s\n"` (0x25cb01724); Create still returns 0 (`mov w22,#0` at 0x25cafda84).
7. No IOConnectSetNotificationPort at create time. It happens later in `IOSurfaceAcceleratorGetRunLoopSource` (1.2).

Create properties dictionary (parsePropertiesHelper 0x25caff76c, keys are CFStrings 0x28625b058/078/098):
| key | type read | default | validated |
|---|---|---|---|
| `PriorityBand` | CFNumber SInt32 (type 3) | 2 | none in userland; kernel rejects >= 4 (see 2.4) |
| `DutyCycleDuration` | CFNumber SInt64 (type 4) | 50000 (0xc350) | < 0xf4241 |
| `HistogramDuration` | CFNumber SInt64 | 500000 (0x7a120) | < 0xf4241 |
Defaults come from the 16-byte constant at 0x25cb01ad8 plus `0x7a120` stored at [sp+0x20] (0x25cafd890). If a key is out of range the whole override is dropped (`cset w0,cc` 0x25caff814).
CoreAnimation passes `PriorityBand` and `DutyCycleDuration` (`CA::IOSAManager::create_new_iosa_manager`, QuartzCore 0x1840791a0, Create call at 0x184079290; DutyCycleDuration = 0x30d40 = 200000).

### 1.2 Notification port (IOSurfaceAcceleratorGetRunLoopSource, IOSA 0x25caff8e0)
Only runs when the client asks for the runloop source (CA does, 0x1840792a8):
1. `IONotificationPortCreate(kIOMainPortDefault)` stored at `obj+0x10`.
2. `IOConnectSetNotificationPort(conn, type=0, IONotificationPortGetMachPort(port), refcon=0)` at 0x25caff938. The return value is ignored by the framework, but the kernel must remember the port for async completion (kernel string `"registerNotificationPort"`, `"notification port already registered!"` in the kext).
3. `IONotificationPortGetRunLoopSource` stored at `obj+0x18` and returned.
So: `IOUserClient::registerNotificationPort(mach_port_t port, UInt32 type, UInt32 refCon)` must be overridden on the client; type is always 0.

### 1.3 Registry properties read from the service
- **IOSurfaceAccelerator create/transfer paths read none.** Neither Create nor TransformSurface touches the registry.
- `IOSurfaceAcceleratorGetHistogram` (IOSA 0x25caffe40): `IORegistryEntrySearchCFProperty(service, "IOService", "IOSurfaceAcceleratorCapabilitiesDict", NULL, 0)` then key `kSurfaceAcceleratorCapabilitiesHistogramValues` (CFNumber SInt32). Histogram only; not needed.
- CoreAnimation reads the same dict through `CA::ScalerUtil::get_io_service`'s service (IORegistryEntrySearchCFProperty(svc, "IOService", CFString "IOSurfaceAcceleratorCapabilitiesDict", NULL, 0), QuartzCore 0x184009ea0 and 0x184084004):
  - `has_capability(key)` (0x184009ea0): dict missing or key missing -> returns 0 (false). Value converted with `CA_CFBoolValue`. Keys CA asks: `IOSurfaceAcceleratorCapabilitiesFilmGrain` (0x18400a160), `IOSurfaceAcceleratorCapabilitiesHDR` (0x18418a1d8), `IOSurfaceAcceleratorCapabilitiesVariableRateDirectionalScaling` (0x18418c21c). Leaving these absent keeps CA on the plain scale path. Do not publish them as true.
  - `ScalerLimits::limits()` block (0x184084004): initializes defaults from constants at 0x1842417b0..0x1842417e0, then overrides each field with `CA_CFIntValue(dict[key])` when the key exists and the value is >= 1 (>= 1 test at 0x184084100 etc.). Defaults (IOSA key order): MinSourceWidth/Height 16/16, MaxSourceWidth/Height 0x4000/0x4000, MinDestWidth/Height 4/4, MaxDestWidth/Height 0x1000/0x1000, then float ratio limits (0.25, 0.25, 4.0, 4.0, 1.0, 1.0, 2.0, 2.0 at 0x1842417d0/0x1842417e0, mapping to MaxH/VDownscale/Upscale INFERRED). 1344x2992 -> 940x2094 is inside the defaults. No property is required; publishing none is safe.
  - Keys CA reads for limits (CFStrings at QuartzCore 0x1ee190d70..0x1ee190f90): `IOSurfaceAcceleratorCapabilities{MinSourceWidth,MinSourceHeight,MaxSourceWidth,MaxSourceHeight,MinDestWidth,MinDestHeight,MaxDestWidth,MaxDestHeight,MaxHUpscale,MaxVUpscale,MaxHDownscale,MaxVDownscale}` plus `IOSurfaceAcceleratorFilter{VerticalTapsCount,VerticalPhasesCount,HorizontalTapsCount,HorizontalPhasesCount,CoefficientsPrePointBits,CoefficientsPostPointBits}`. Values are CFNumbers (kernel strings: `"Capability/Extent OSNumber create for %s Failed"`).
- The real kernel publishes `IOSurfaceAcceleratorCapabilitiesDict` via `propertyExport` (kext strings). Dict contents include per-format strings (`IOSurfaceAcceleratorFormatIn*/Out*`) and the numeric limits. Other clients (VideoToolbox, CMCapture, AppleCVA, ASEProcessing) read it; backboardd path does not need them.
- Also published by the kext as writable runtime properties (`setProperties`): `hardware-version`, `workload-priority`, `direct-dart-power`, `simulated-model`, `active-mcc-bit-vector` etc. (kext strings near `registerRuntimeProperties`). Not read by the framework paths analysed here.

### 1.4 Why CSC vs Paravirt
Only `kern.hv_vmm_present` (+ `debug.iosa.simulated_model`) decides, in IOSA (above). On our bare-metal xnu: CSC. Shim `AppleM2ScalerCSCDriver`.

---------------------------------------------------------------------------

## 2. IOConnectCall* made by IOSurfaceAccelerator.framework

All use `IOConnectCallStructMethod` (or ScalarMethod for the aborts) on `conn = obj+0x24`. IOConnectCallStructMethod passes the struct as `structureInput` and optional output; no scalar arrays. There are no async IOConnectCall* variants and no `IOConnectCallAsync*` (no such import in the dylib; the "async" model is the callback fields inside the struct, see 2.2).

| sel | name (kernel string) | in | out | framework caller (IOSA) | kernel table (3.1) |
|---|---|---|---|---|---|
| 0 | capture | struct 0x40 | none | `IOSurfaceAcceleratorCaptureSurface` 0x25caffaf8, call 0x25caffbe8 | StructI var-size (0xffffffff) |
| 1 | `user_transform_surface` | struct 0x1b0 | none | transformSurface 0x25cafeb6c, tail call 0x25cafed04 (and `ParavirtProcessGuestData` 0x25cb002cc) | StructI 0x1b0 |
| 2 | abort transfers | scalar 0 | scalar 0 | `IOSurfaceAcceleratorAbortTransfers` 0x25caffa7c | 0/0 |
| 3 | abort captures | scalar 0 | scalar 0 | `IOSurfaceAcceleratorAbortCaptures` 0x25caffadc | 0/0 |
| 4 | `user_set_filter` | struct 0x20 | none | `IOSurfaceAcceleratorSetCustomFilter` 0x25cafedd0 (call 0x25cafee34) | StructI 0x20 |
| 5 | (unnamed, driver vfunc +0x548 -> scalar) | scalar 0 | scalar 1 | not called by IOSA | 0/1 |
| 6 | kernel tests | struct 0xfa8 | none | `IOSurfaceAcceleratorKernelTests` 0x25caffe0c | StructI 0xfa8 |
| 7 | `user_get_histogram` | struct 8 | none | `GetHistogram` 0x25caffe40 (call 0x25caffee4) | StructI 8 |
| 8 | `user_get_iosaDiag` | struct 8 | none | `GetDiag` 0x25cafff5c (call 0x25cafffbc) | StructI 8 |
| 9 | `user_get_estimation` | struct 0x10 | none | `GetTransformEstimation` 0x25caffff8 (call 0x25cb00104) | StructI 0x10 |
| 0xa | `user_set_client_property` | struct 0x18 | none | Create 0x25cafda60; `SetProperty` 0x25cb00134 (call 0x25cb001e8) | StructI 0x18 |
| 0xb | `user_get_capabilities` | none | struct 0x288 (`SCALER_PROPERTIES_EXTENTS`) | `ParavirtProcessGuestData` type 1, 0x25cb00280 (output ptr x4 = caller buf, size ptr x5 = caller+0x18) | StructO 0x288 |
Not used by the transfer path: 0, 3, 4, 5, 6, 7, 8, 9, 0xb. Selector 2 is invoked by `AbortTransfers` only (not by Create/Finalize that was observed; finalize 0x25cafeec0 not disassembled, INFERRED it closes with IOServiceClose and may call 2).

### 2.1 Entry points and argument order
- `IOSurfaceAcceleratorTransferSurface(accel, src, dst, options, completion, ?, ?)` at 0x25caff960: forwards to `convertToTransform(accel, src, dst, options, rectsrc=NULL, rectdst=NULL, x4_in, x5_in, [stack]=x6_in)`. Registers x4/x5/x6 of the public call become completion callback/refcon/third (see 2.2).
- `IOSurfaceAcceleratorTransformSurface(accel, src, dst, options, rects*, completion*)` at 0x25cafdae8: `rects` points to 0x30 bytes (copied by `prepareTransformRectangles` 0x25cafdbb4); `completion` points to 0x18 bytes {callback, refcon, third}.
- `IOSurfaceAcceleratorBlitSurface(accel, src, srcRect*, dst, dstRect*, options, cb, refcon, [stack third])` at 0x25caffabc remaps registers into `convertToTransform` with rect pointers.
- `TransferSurfaceWithSwap`, `BlitSurfaceWithSwap` (0x25caffa98, 0x25caffad4) are stubs returning 0 without calling the kernel.
- `ConditionalTransferSurfaceWithSwap` 0x25caffaa0 == TransferSurface.
- All three paths then run `prepareTransformBuffersAndOptions` (0x25cafdbd0) to fill the 0x1b0 struct and `transformSurface` (0x25cafeb6c) to send it. The IOReturn from the kernel is returned to the caller; if prepare fails it returns `0xe00002c2` (bad argument) or the option error and never calls the kernel.
- CoreAnimation's actual calls: `CA::WindowServer::IOSurface::copy_iosurface` (QuartzCore 0x184060474, `IOSurfaceAcceleratorTransformSurface` stub call at 0x184061238) and `clear_iosurface` (`IOSurfaceAcceleratorBlitSurface` call at 0x184168dd8). In the user's logs the green 420f snapshots come from copy_iosurface.

### 2.2 Selector 1: the 0x1b0 transform struct ("TransformSurfaceData")
Built by IOSA in `prepareTransformBuffersAndOptions` (0x25cafdbd0) from the dictionary and surfaces, plus rect and completion words written by convertToTransform (0x25caff998) / TransformSurface (0x25cafdae8). The kernel handler checks `size == 0x1b0` (user_transform_surface at 0xfffffff008979c94, error string `"given size (%llu) doesn't match TransformSurfaceData's (%lu)"`, returns 0xe00002c2).

Surface naming: `prepare(x0=src, x1=dst)`. Surfaces are identified by IOSurface ID:
- `+0x00 u32 srcID` = `IOSurfaceGetID(src)` (0x25cafdc28..0x25cafdc2c). INFERRED from argument order: prepare is called with (src, dst) by convertToTransform (x0=x1_in, x1=x2_in) and by TransformSurface (x0=x22=x1_in, x1=x21=x2_in).
- `+0x04 u32 dstID` = `IOSurfaceGetID(dst)` (0x25cafdc1c).
IDs are the kernel-side IDs: our HuskySurface create_surface output at offset 0x18 returns `s->id` (HuskySurface.cpp:696) and the userland IOSurfaceGetID returns that stored value. Resolve with `husky_surface_copy(id)`.
No mach ports for the pixel surfaces. Shared events (below) are the only ports.

| off | size | field | source / meaning |
|---|---|---|---|
| 0x00 | u32 | srcID | see above |
| 0x04 | u32 | dstID | see above |
| 0x08 | u64 | completion callback (user VA) | 0 = synchronous (kernel path at 0x8979740 `ldr x8,[x1,#8]; cbz`). From `completion[0]` / public x4 |
| 0x10 | u64 | completion refcon (user VA) | `completion[1]` |
| 0x18 | u64 | completion third word / arg0 | `completion[2]`; echoed back as the async result argument (kernel stores `[x21+0x18]` into its async object, 0x8979920) |
| 0x20 | u64 | flags | bitfield, table below |
| 0x28 | u64 | src rect X | 16.16 fixed point (see 2.3); 0 when no rect given |
| 0x30 | u64 | src rect Y | 16.16 |
| 0x38 | u64 | src rect width | 16.16 |
| 0x40 | u64 | src rect height | 16.16 |
| 0x48 | u32 | src surface width | `IOSurfaceGetWidth(src)` at 0x25cafdc38 (`[x19+0x48]=Width(x23)`, x23 = prepare's x0 = src) |
| 0x4c | u32 | src surface height | `IOSurfaceGetHeight(src)` at 0x25cafdc44 |
| 0x50 | u64 | zero | `str xzr,[x19,#0x50]` (0x25cafdc60) |
| 0x58 | u64 | zero | cleared at 0x25cafdc14 |
| 0x60 | 16 B | dst rect {x,y,w,h} as 4 x u32 | from `rects[0x20..0x2f]` or `dstRect*`; INFERRED layout (CA builds 4 words at fp-0xb0/-0xa8, QuartzCore 0x184060c50..0x184060c58) |
| 0x70 | u32 | dst surface width | `IOSurfaceGetWidth(dst)` (0x25cafdc50) |
| 0x74 | u32 | dst surface height | `IOSurfaceGetHeight(dst)` (0x25cafdc58) |
| 0x78 | u32 | src shared-event mach port | `IOSurfaceSharedEventGetMachPort(options[SharedEventSrcRef])` (0x25cafe560) |
| 0x80 | u64 | src event wait value | `SharedEventSrcWait` |
| 0x88 | u64 | src event signal value | `SharedEventSrcSignal` |
| 0x90 | u32 | dst shared-event mach port | `SharedEventDstRef` (0x25cafe5b0) |
| 0x98 | u64 | dst event wait value | `SharedEventDstWait` |
| 0xa0 | u64 | dst event signal value | `SharedEventDstSignal` |
| 0xa8 | u32 | alpha override | `AlphaOverride` (when set, flags bit 18) |
| 0xac..0xb0 | 2 x u32 | border fill (x? y?) | `BorderFillX/Y` stored via `stp w8,w9,[x19,#0xac]` (0x25cafe0a4); order INFERRED |
| 0xb4..0xb8 | 2 x u32 | border fill width/height | stp at 0x25cafe0ac; order INFERRED |
| 0xbc | u32 | border fill alpha / odd-size alpha | `BorderFillAlpha` (0x25cafe0dc) or `BorderFillOddSizeYCbCrAlpha` |
| 0xc0..0xc8 | u32 x3 | border fill Red/Y, Green/Cb, Blue/Cr (+odd-size luma at 0xc0) | stp/str at 0x25cafe0b4..0x25cafe0bc |
| 0xcc | u16 x4 | WriteOnlyMode alpha / red-luma / green-cb / blue-cr | strh at 0xcc/0xce/... (0x25cafdf5c, 0x25cafdf7c ...) |
| 0xdc | u32 | histogram bin mode | `HistogramBinMode` |
| 0xe0,0xe4,0xe8,0xec | u32 x4 | histogram X, Y, width, height | |
| 0xf0 | u32[<=8?] | histogram pixel bins | bins array (< 0x10000 each) |
| 0x110 | 4 x 0x28 B | "Comm API" entries | each: u32 type @+0, u64 @+8, u32 @+0x10, u32 @+0x14, u64 @+0x18, u32 @+0x20; filled from `IOSurfaceAcceleratorCommApi` CFArray of CFData (0x28 bytes each, 0x25cafe5fc..0x25cafe738). Zero unless HDR/ASE callers use it. Struct end = 0x110 + 4*0x28 = 0x1b0 |

Flags (u64 at +0x20), from prepareTransformBuffersAndOptions (0x25cafdbd0), key = CFString in the options dictionary:
| bit(s) | option key | rule |
|---|---|---|
| 13..14 | `DitherControl` | CFNumber 0..3 (>=4 -> error 0xe00002c2), default 1 when dict is NULL or key absent: flags |= v<<13 (0x25cafde18; default also `orr #0x2000` at 0x25cafdd0c) |
| 0..1 + one more | `Transform` | CFNumber T; `(T & ~3)` must be 0 or a power of two else error; flags |= T (0x25cafddc8). T=0: none. Bit meanings (flipX/flipY/rot90) INFERRED, kernel error string `"Invalid flip/rotate settings: 0x%x"` |
| 15..17 | dst chroma siting | from dst `kIOSurfaceChromaLocationTopField` or bulk attachments, values 1..6 (Left=1, Center=2, TopLeft=3, Top=4, BottomLeft=5, Bottom=6), flags |= v<<15 (0x25cafe748), applied only for non-"w21" case |
| 6 | `CustomFilter` | bool |
| 7 | `NearestFilter` | bool (CA sets from its own flag, QuartzCore 0x1840606b4) |
| 8 | `OptOutBlitClip` | bool |
| 9 | `LockInScaler` | bool |
| 10 | `SkipAsyncCallback` | bool (kernel reads `(flags>>10)&1`, 0x8979928; with it set the callback is not delivered) |
| 12 | `FixUpscaling` | bool; also set implicitly (`orr #0x1000`, 0x25cafe764) for non-blit calls when w21==0 |
| 18 | `AlphaOverride` | CFNumber -> +0xa8 |
| 28 | border fill set | all of BorderFill{X,Y,Width,Height,RedY,GreenCb,BlueCr} present |
| 29 | histogram | `HistogramBinMode` present |
| 30 | `ZeroFill` | CFNumber != 0 |
| 31 | `DisablePseudoLinearScaling` | |
| 32 | `PixelAveraging` | |
| 33 | `EdgeMirroring` | |
| 35.. | `UnwireSurface` | CFNumber << 35 (0x25cafdc98) |
| 36.. | `ColorRemapMode` | CFNumber < 5, << 36 (0x25cafe240); CA passes `kDisplayColorRemapModeKey` |
| 39 | `DirectionalScalingEnable` | |
| 40 | `HDREnable` | |
| 41 | `EnhanceEnable` | |
| 42 | `NoiseEstimationEnable` | |
| 43 | `ReadOnly` | |
| 44 | `HtpcLargeHeaders` | |
| 45 | `VraScaling` | |
| 46 | `NoPowerEarlyBail` | |
| 47 | `FirmwareTimeoutInject` | |
| 48 | `ITUVariantCSCEnable` | |
| 49 | `WriteOnlyMode*` (Alpha/RedLuma/GreenCb/BlueCr) | |
| 50 | `FilmGrainEnable` | |
| 51 | `ReducedPrecisionFractionalOffsets` | |
| 52 | `NoCoalesceAsyncIOFenceWrites` | |
| 53 | `DestinationSurfaceDisplayBound` | CA sets (QuartzCore 0x184060744) |
| 54 | `SymmetricScaling` | |
| 55 | `DefaultChromaDownsample` | |
| 57 | `BorderFillOddSizeYCbCrEnable` | |
`HighPriority` handling (0x25cafe3b4..0x25cafe3c8) calls a cold path; effect on the struct not decoded.
CoreAnimation sets: `Transform` (SymmetricTransformKey), `NearestFilter` (conditional), `ColorRemapMode`, `DestinationSurfaceDisplayBound`, optionally `EnhanceEnable`, `NoiseEstimationEnable`, `DirectionalScalingEnable`, and SharedEvent keys (QuartzCore 0x184060694..0x184060750). None of them changes the required output of a plain scale/convert; a shim may ignore all flags other than Transform and nearest/linear filtering.

Completion fields: the framework never reads the struct back (no output), so any ID/handle the kernel writes to `+0x10` (kernel does `stp x2,xzr,[x21,#0x10]` at 0x8979970 in the K2K path) is not visible to userland.

### 2.3 Rectangles
`convertToTransform` (0x25caffa04..0x25caffa38), used by BlitSurface when rect pointers are given. Input rect struct R (0x18 bytes, INFERRED type): `{i32 x, i32 y, u32 w, u32 h, u32 fx, u32 fy}`:
- `+0x28 = (R.fx<<14) | (R.x<<16)`; `+0x30 = (R.fy<<14) | (R.y<<16)` (stp at 0x25caffa24). R fields: `ldp w8,w9,[x4,#0x10]` = (fx,fy), `ldp w10,w11,[x4]` = (x,y).
- `+0x38 = R.w<<16`, `+0x40 = R.h<<16` (`ushll v0.2d,v0.2s,#0x10` of `[x4+8]`, 0x25caffa2c).
- `+0x60..0x6f = 16 raw bytes from dstRect*` (`ldr q0,[x5]; str q0,[sp,#0x60]`, 0x25caffa38).
`TransformSurface` instead takes a 0x30 byte `rects*` copied verbatim: bytes 0x00..0x1f -> `+0x28..+0x47` (src x,y,w,h as four u64 already in 16.16 fixed), bytes 0x20..0x2f -> `+0x60` (dst rect, four u32). `prepareTransformRectangles` 0x25cafdbb4. CA fills this from doubles via `fcvtzu v0.2d,#0x10` (QuartzCore 0x184060c3c): X, Y, W, H = value*65536. Confirms 16.16.
`TransferSurface` (no rect args) leaves all rect words 0: **zero rects mean "whole surface"** (INFERRED; the kernel size/offset checks use the +0x48/+0x4c, +0x70/+0x74 surface dimensions). Treat src rect w/h == 0 as srcWidth/srcHeight and dst rect w/h == 0 as dstWidth/dstHeight.
Rotation/flip: only through `Transform` (flags low bits). Scale factor is implied by src rect size vs dst rect size.
Colour conversion: **not an option**. It is implied by the pixel formats of the two surfaces. Full vs video range follows the 420f/420v pixel format (INFERRED); the YCbCr matrix comes from surface attachments the kernel reads itself (the kext has `getIOSurfaceFilmGrainAttachment` and ColorManager classes; userland IOSA only reads chroma siting, see flags 15..17).

### 2.4 Selector 0xa: client property (0x18 bytes)
Layout (IOSA `parsePropertiesHelper` result copied from [sp+0x10], sent at 0x25cafda60): `{u32 priorityBand @0 (default 2), u64 dutyCycleDuration @8 (default 50000), u64 histogramDuration @0x10 (default 500000)}`.
Kernel `user_set_client_property` (handler 0xfffffff00897a6c4): size must be 0x18 else 0xe00002c2; `priorityBand < 4` else 0xe00002c2 (string `"schedulingPriority (%u) out of range [0, %u)"`); stores band at client+0x158 and the 16 bytes at +8 to client+0x160. A shim can validate and ignore.

### 2.5 Selector 0 (capture, 0x40 bytes) for completeness
Struct (IOSA 0x25caffb2c..0x25caffb70): `+0x10..0x27` = 0x18 bytes of caller options (completion triple), `+0x28` = Transform<<1, `+0x30 u32 srcID` (IOSurfaceGetID of arg), `+0x34 u32 width`, `+0x38 u32 height`, `+0x3c..` rect offsets (x,y,w,h). Not used by CoreAnimation snapshots; return kIOReturnUnsupported.

### 2.6 Async completion protocol (selector 1 with callback != 0)
Framework side:
- The caller supplies `cb(void *refcon, IOReturn result, void *arg0)`; CA's is `CA::WindowServer::msr_transfer_complete` (QuartzCore 0x18405f0b4: x0=refcon, w1=result, x2=arg0 pointer; it dereferences `x2` as {+0,+8,+0x10}).
- IOSA's own object has the IONotificationPort set up in 1.2; the runloop source dispatches async mach messages via the standard `IODispatchCalloutFromMessage` (INFERRED: the dylib does not install a custom callout; the source returned is `IONotificationPortGetRunLoopSource`).
- `IOReturn` of the selector-1 call itself is the submit result. With a callback the kernel returns immediately after queueing.
Kernel side (kext, user_transform_surface path at 0xfffffff008979740/0x897987c, error strings `"transformSurface_asynchronous"`, `"asynchronousUserClientCompletionCallback"`, `"sendAsyncResult64 failed with 0x%x"`):
- `callback == 0` -> synchronous: runs the transform under the command gate and returns its IOReturn (0x8979758 path).
- `callback != 0` -> asynchronous: allocates a `UserAsyncObjectsWrapper` carrying `[x21+0x18]` (third word), the client, and the SkipAsyncCallback bit (`(flags>>10)&1`) (0x8979910..0x8979930). On completion `asynchronousUserClientCompletionCallback` does `sendAsyncResult64`. The asyncRef array (OSAsyncReference64) uses the registered wake port (client+0x118) and the callback/refcon words from the struct (INFERRED; setAsyncReference64 call 0x8979918 with x1=[client+0x118], x4=[client+0x128] that I could not fully resolve).
- Required shim behaviour (INFERRED, standard IOKit async): build `OSAsyncReference64` with `setAsyncReference64(ref, wakePort, callback=struct+8, refcon=struct+0x10)`, then after the work call `sendAsyncResult64(ref, kIOReturnSuccess, &arg0, 1)` with `arg0 = struct+0x18`. IODispatchCalloutFromMessage then calls `callback(refcon, result, arg0)`, which is exactly msr_transfer_complete's signature.
- If the shim posts no completion, CA leaks the IOSurface-in-flight state (its manager's pending count never drops). So either deliver the completion message or reject async mode (kIOReturnUnsupported makes CA log an error and skip the snapshot; not acceptable).

---------------------------------------------------------------------------

## 3. Kernel side (AppleM2ScalerCSCDriver kext, iOS 26.5)

### 3.1 externalMethod dispatch table
Old-style `IOExternalMethod` array (0x30 byte entries: object, IOMethod ptr+adj, flags, count0, count1) in `__DATA_CONST` of the kext at kext vmaddr 0xfffffff007d25418 (file offset 0x1af238 in the extracted kext; entry 0 starts there). Flags: 0 = scalarI/scalarO, 2 = scalarI/structO, 3 = structI/structO. Handler pointers are chained-fixup targets relative to 0xfffffff007004000 (verified: handlers resolve into the kext's __TEXT_EXEC and decode sensibly).
| sel | flags | count0 | count1 | handler (unslid) | role |
|---|---|---|---|---|---|
| 0 | 3 | 0xffffffff (var struct in) | 0 | 0xfffffff00897a37c | capture |
| 1 | 3 | 0x1b0 | 0 | 0xfffffff008979c74 | `user_transform_surface` |
| 2 | 0 | 0 | 0 | 0xfffffff00897a38c | abort transfers |
| 3 | 0 | 0 | 0 | 0xfffffff00897a39c | abort captures |
| 4 | 3 | 0x20 | 0 | 0xfffffff00897a120 | `user_set_filter` |
| 5 | 0 | 0 | 1 | 0xfffffff00897a0c4 | returns a driver virtual (vtable +0x548) in scalar out[0], gated on `[this+0x154]==0`; unused by IOSA |
| 6 | 3 | 0xfa8 | 0 | 0xfffffff00897a328 | kernel tests |
| 7 | 3 | 8 | 0 | 0xfffffff00897a180 | `user_get_histogram` |
| 8 | 3 | 8 | 0 | 0xfffffff00897a420 | `user_get_iosaDiag` |
| 9 | 3 | 0x10 | 0 | 0xfffffff00897a4fc | `user_get_estimation` |
| 10 | 3 | 0x18 | 0 | 0xfffffff00897a6c4 | `user_set_client_property` |
| 11 | 2 | 0 | 0x288 | 0xfffffff00897a760 | `user_get_capabilities` (size mismatch -> 0xe00002c2, string `"given size (%llu) doesn't match SCALER_PROPERTIES_EXTENTS (%lu)"`) |
Sizes match the framework's calls exactly (2: 1 -> 0x1b0 (also the SRS path uses 0x58 against a different driver), 4 -> 0x20, 6 -> 0xfa8, 7/8 -> 8, 9 -> 0x10, 0xa -> 0x18). Selector 0 variable size accepts the framework's 0x40.
Note that the struct inputs in this table are `kIOUCStructIStructO` with output size 0, i.e. the user-space call must pass no output buffer (IOSA passes NULL/NULL for all but selector 0xb).

### 3.2 Which selectors a basic synchronous transfer needs
- Needed for IOSurfaceAcceleratorCreate to succeed: only IOServiceMatching + IOServiceOpen (type 0). Selector 0xa failure is non-fatal (message only), but returning success is cleaner.
- Needed for TransferSurface/TransformSurface/BlitSurface: selector 1.
- Needed for CA's async mode: `registerNotificationPort` (type 0), and `sendAsyncResult64` on completion.
- Can simply return success (or not exist): 2 (abort), 3, 0xa (accept bands < 4), 5 (return 0).
- Can return kIOReturnUnsupported: 0, 4, 6, 7, 8, 9, 0xb. CA calls none of them in copy_iosurface / clear_iosurface paths (INFERRED: the only IOSA symbols QuartzCore imports are Create, TransformSurface, BlitSurface, GetRunLoopSource, GetTransformEstimation; the last is not in the paths traced here, so keep selector 9 returning a zeroed 0x10 buffer to be safe).
- `kIOUserClientClassKey`: published service must make `IOServiceOpen` instantiate a user client; the real kext defines class `IOSurfaceAcceleratorClient` (kext string, `initClient`, "cannot find the scaler driver"). The userland ignores class names.
- No entitlement string appears in the kext (no `com.apple.*` entitlement checks found by string search); only a literal `backboardd` string (likely a process-name tag). INFERRED: any task may open it.

### 3.3 Registry properties the service publishes (kext `propertyExport`)
- `IOSurfaceAcceleratorCapabilitiesDict`: OSDictionary of OSNumber/OSString/OSBoolean (kext strings `"Capability/Extent OSNumber create for %s Failed"`, `"...OSString..."`, `"...Dictionary set for %s Failed"`). Key names (all in the kext): `IOSurfaceAcceleratorCapabilities{MinSourceWidth,MinSourceHeight,MinAseSourceWidth,MinAseSourceHeight,MinDestWidth,MinDestHeight,MaxSourceWidth,MaxSourceHeight,MaxAseSourceWidth,MaxAseSourceHeight,MaxDestWidth,MaxDestHeight,MaxHUpscale,MaxHDownscale,MaxVUpscale,MaxVDownscale,FractionalXYOffsetsSupported,SourceXYOffsetsWithTiling,DestXYOffsetsWithTiling,DirectionalScaling,HDR,FilmGrain,...}`, `IOSurfaceAcceleratorFormatIn*/Out*` strings, filter keys, `kSurfaceAcceleratorCapabilitiesHistogram*`.
- Writable debug properties on the driver (iosa sysctl-like: `iosa.idle_power_off_us`, `iosa.simulated_model`, ...); not read by clients here.
- The framework needs none of them for transfer (1.3). CA is fine with no dictionary at all (all boolean capabilities read false; limits fall back to defaults).

---------------------------------------------------------------------------

## 4. Minimal shim plan

Service: an IOService whose class (or a superclass) is named `AppleM2ScalerCSCDriver` so `IOServiceMatching("AppleM2ScalerCSCDriver")` (class-name match) finds it, `registerService()`d, with `kIOUserClientClassKey` = a client class (name free; mirror `HuskySurface.cpp`'s `IOCoreSurfaceRoot` pattern at lines 498-520). Provide `newUserClient` accepting type 0 and ignoring `type != 0` except to fail if you want SRS; userland only opens type 0.

User client implements, via `externalMethod` (or `getTargetAndMethodForIndex` with the table above):
1. **sel 1** (struct in 0x1b0, struct out 0, otherwise `kIOReturnBadArgument` 0xe00002c2):
   - Read `srcID = u32@0x00`, `dstID = u32@0x04`; `husky_surface_copy` both (retained; release at end). If either is NULL return `kIOReturnBadArgument`.
   - Src rect = (x,y,w,h) = u64@0x28,0x30,0x38,0x40, each >> 16 (fixed 16.16; keep the fraction if you want subpixel). w==0 or h==0: whole src (u32@0x48,@0x4c). Dst rect = u32[4] @0x60; w==0 or h==0: whole dst (u32@0x70,@0x74).
   - Flags (u64@0x20): bit 7 nearest filter; `Transform` low bits (0..1 plus one more; unsupported value -> kIOReturnUnsupported); ignore the rest.
   - Pixel formats from `HuskySurface::pixelFormat` and planes[]: source BGRA (`'BGRA'`) read at (bytesPerRow, offset); destination:
     - `'420f'` (full range) and `'420v'` (video range): 2 planes, Y plane `planes[0]` (1 B/px), CbCr plane `planes[1]` (2 B per 2x2 px, interleaved Cb then Cr). Convert with BT.709 (matrix choice INFERRED: CA tags the destination; if HuskySurface has no attachment store, default BT.709) and range scaling Y 16..235/C 16..240 for 420v, 0..255 for 420f.
     - `'BGRA'`->`'BGRA'`: plain bilinear/nearest scale (no CSC) honouring dst/src rects and nearest-filter flag. Also add 'RGBA' if seen. (Whether CA uses scale-only mode is INFERRED from `fourcc` handling in copy_iosurface; format list in the kext: Out ARGB8888/RGBA8888/2 planes 420 etc.)
     - Anything else: kIOReturnUnsupported.
   - Pixel access: `HuskySurface::kernelAddress()` (maps `memory` into the kernel), honour `offset`/`planes[i].offset`; after writing call `husky_dcache_flush`/clean as the surface code does and bump the write seed if you want scanout/consumers to see the change (surfaces bump seed on unlock only; HuskySurface::seed()).
   - Sync (callback == u64@0x08 == 0): do the work, return `kIOReturnSuccess`.
   - Async (callback != 0): do the work inline, return `kIOReturnSuccess` from the call, then post the completion: `OSAsyncReference64 ref; setAsyncReference64(ref, wakePort_from_registerNotificationPort, callback=u64@0x08, refcon=u64@0x10)` and `sendAsyncResult64(ref, kIOReturnSuccess, &arg0(=u64@0x18), 1)`; skip if flags bit 10 (`SkipAsyncCallback`) is set. Posting must happen after the externalMethod returned or from a thread call (do not hold the command gate); posting before return also works on stock IOKit but ordering relative to the user's own bookkeeping is untested (INFERRED).
2. **registerNotificationPort(port, type, refCon)**: store `port` (type 0 only) for async results; replace an existing registration without erroring (framework registers once).
3. **sel 0xa**: size 0x18, `u32 band < 4` else bad argument; otherwise success. **sel 2, 3**: success. **sel 5**: scalar out 0. **sel 9**: zero-fill 0x10 B, success. **sel 0, 4, 6, 7, 8, 0xb**: `kIOReturnUnsupported`.
4. `clientClose`/`didTerminate`: drop the port, and complete nothing further.

Properties: none strictly required. Recommended (harmless, stops CA from taking odd branches): publish `IOSurfaceAcceleratorCapabilitiesDict` as an OSDictionary on the service with at most the limit keys (MinSource 16x16 / MaxSource 16384x16384 / MinDest 4x4 / MaxDest 4096x4096 -> the defaults CA already uses; raise MaxDest if the 1344x2992 snapshot is not to be rejected: it fits the defaults). Do NOT publish `...CapabilitiesHDR`, `...FilmGrain`, `...VariableRateDirectionalScaling` as true.

Survival checklist for a first boot of the shim:
- backboardd log `IOServiceMatchingfailed for: AppleM2ScalerCSCDriver` disappears; `CoreAnimation: Failed to create IOSurfaceAccelerator: 0x...` (QuartzCore string at 0x1842855b2) disappears.
- `ScalerUtil::get_io_service` then caches a non-zero service and goes on to read the caps dict (absent -> defaults).
- First selector-1 call will have srcID = BGRA snapshot source, dstID = new 420f 940x2094 or 1344x2992 surface, flags with default dither (bit 13 set, 0x2000), rects in 16.16.

---------------------------------------------------------------------------

## 5. Remaining unknowns (ranked)

1. Exact `OSAsyncReference64` population in the real kernel (who supplies callback/refcon: the struct words at +8/+0x10 vs values registered through `IOConnectSetNotificationPort`'s refcon (0)). The standard IOKit pattern plus msr_transfer_complete's signature strongly suggest the former, but I did not trace `setAsyncReference64` args. First boot test: log the async result send and confirm msr_transfer_complete runs (CA's `pending` count).
2. YCbCr matrix/range source for the destination. The kext reads IOSurface attachments itself; I did not find which key. Pixel format (420f vs 420v) gives range. Matrix default BT.709 is a guess; check what CA sets on the destination (kIOSurfaceYCbCrMatrix attachment) and what HuskySurface stores.
3. `Transform` bit meanings (flip vs rotate) and whether CA's snapshot path ever sets them.
4. Meaning of `PriorityBand` constants CA passes and the `[sp+0xc]` value (only matters for the kernel scheduling; harmless).
5. The struct fields `+0xac..+0xc8` (border fill) ordering and `+0xcc..` WriteOnly layout: only exercised by clear/fill paths; `clear_iosurface` uses BlitSurface with a source surface and rects (QuartzCore 0x184168dd8), not WriteOnlyMode keys, so the basic fill works through the same selector 1.
6. Whether `IOSurfaceAcceleratorFinalize` (0x25cafeec0) calls selector 2 before `IOServiceClose` (not disassembled).
7. Whether CA ever pre-checks the capability dict for formats (e.g. `IOSurfaceAcceleratorFormatOut2Planes420`). Only the three boolean capabilities and the limits were found in QuartzCore; other consumers (AppleCVA, VideoToolbox) may check format strings if they run on-device.
8. Whether any process in the boot flow (SpringBoard, FrontBoard) calls IOSA directly in addition to CA. The IOSA importer list (ipsw dyld imports) in the DSC covers QuartzCore, ImageIO, VideoToolbox, MediaToolbox, WebCore, CMPhoto, Vision, MobileIcons, etc.; backboardd/SpringBoard binaries themselves live in the filesystem DMG and were not scanned.
