# Husky display: 120 Hz zero-copy scanout of CoreAnimation IOSurfaces (kernel design)

Inputs: `facts-decon-dpp.md` (FD), `facts-dsim-panel.md` (FP), `facts-integration.md` (FI).
Cites `FD §n` etc. refer to those files; ground-truth cites are to the google-modules / devices
checkouts named there. Register values below are facts from those sources or from silicon; all
code is to be written fresh (GPL sources were read for facts only).

Kernel tree: `/home/nhansp/aosp/ios-husky/out/xnu-display-wt` (K). **Prerequisite P0:** K is at
`base 0001-0120`; IOGPU patches 0121 and 0122 (IOSurface import through
`md->getPhysicalSegment`, `artifacts/xnu-12377/iokit/Families/IOGPU/IOGPUMemory.cpp:190,317`)
must be applied to K first. Without 0122 the GPU never writes into IOSurface pages, so nothing
GPU-rendered can be scanned out.

---

## 1. Decisions at a glance

| Topic | Decision |
|---|---|
| Frame start | DECON0 **HW trigger from TE (DDI0)**, kept exactly as ABL/shim left it (`TRIG_CON` HW_TRIG_EN=1, SEL=DDI0, bit4 mask clear). One frame per shadow-update request (silicon fact). No SW trigger. |
| Flip | Write `RDMA0 BASEADDR_Y8` (0x19900040) = surface PA, `DSB SY`, `GLOBAL_CON |= 0x3`, `SHD_REG_UP_REQ = 0x80003FFF` (the proven `husky_fb_kick` value). DECON latches at the next TE (FRAME_START) and transfers (~5.9 ms). Nothing else in DECON/DPP/DSC/DSIM is reprogrammed. |
| Scanout memory | **Physically contiguous scanout pool below 4 GiB**, DPU SysMMU stays in bypass. CA's framebuffer IOSurfaces are allocated from the pool at create time. Anything else falls back to a CPU copy into the splash buffer. |
| In flight | At most 2 swaps owned by the display: one ARMED (requested, waiting for TE) and one SCANNING (latched, transferring). A third SwapEnd blocks (≤ 50 ms) until ARMED latches. |
| Swap complete | A swap is retired at its own FRAME_DONE (command mode: panel GRAM now holds it, DPU no longer reads it). SwapWait (sel 6) blocks until the named swap is retired. |
| Vsync | Notification type 5 on every TE rising edge, taken from DECON0 `INT_PEND_EXTRA.TE_RISE` (EXTRA IRQ, SPI 241 = INTID 273). The 60 Hz IOTimerEventSource stays only as the fallback source. |
| IRQs | New GIC SPI enable/route helper (pe_fiq.c) + per-INTID dispatch table in `sleh_irq` (HUSKY branch). ISR does register ack + state transitions under a spin lock; all Mach messaging and sleeping happens on the AppleCLCD workloop. |
| 120 Hz | Panel: hk3 manual 120 Hz, HS op, early-exit on, fixed TE (DCS sequence in §8, from `panel-google-hk3.c`, the real husky driver, FP §0). DSIM: rewrite `UNDERRUN_CTRL` and `CMD_TE_CTRL1` for 120 fps. DECON: nothing. |
| Failure | Sticky fallback to the current path (CPU copy into splash at 0xfac00000 + kick), vsync from TE if alive else timer. 120 Hz failure reverts the panel to 60 Hz. |
| Gate | `huskydisp=N` (cumulative, §11). Absent or 0 = today's behavior, no new code runs. |

---

## 2. Hardware model used by this design

Addresses (DT, FD §1; all used regions fit in one 16 KiB page each, mapped with `ml_io_map`):

| Block | PA | Used registers |
|---|---|---|
| DECON0 main | 0x19470000 | GLOBAL_CON 0x20, TRIG_CON 0x30, HW_TE_CNT 0x38, SHD_REG_UP_REQ 0x50, INT_EN 0x60, INT_EN_EXTRA 0x64, INT_PEND 0x70, INT_PEND_EXTRA 0x74; shadow copy at +0x800 |
| DECON win / wincon | 0x19480000 / 0x194A0000 | read-only probe (window enable, CHMAP, position), per window +0x1000 |
| DPP0 RDMA (L0) | 0x19900000 | RDMA_ENABLE 0x00, RDMA_IRQ 0x04, IN_CTRL_0 0x08, SRC_SIZE 0x10, SRC_OFFSET 0x14, IMG_SIZE 0x18, BASEADDR_Y8 0x40, SRC_STRIDE_0/1 0x50/0x54; shadow bank at +0x400 |
| DSIM0 | 0x19440000 | ESCMODE 0x2C, UNDERRUN_CTRL 0x34, CONFIG 0x4C, INTSRC 0x50, PKTHDR 0x58, PAYLOAD 0x5C, FIFOCTRL 0x68, CMD_CONFIG 0x80, CMD_TE_CTRL0/1 0x84/0x88, CMD_STATUS 0x8C, PLL_CTRL 0xA0 |
| DPU SysMMU F0 | 0x19840000 | read-only probe of `REG_MMU_CTRL_VM` 0x8000 bit0 (must stay 0) |

Bits (FD §2, §5; FP §5):
- `INT_EN` 0x60: INT_EN bit0, INT_EN_EXTRA bit4, FRAME_START bit12, FRAME_DONE bit13. `INT_PEND` 0x70 same positions, W1C.
- `INT_EN_EXTRA` 0x64: TE_RISE bit16, RESOURCE_CONFLICT bit4, TIME_OUT bit0. `INT_PEND_EXTRA` 0x74 same, W1C.
- `RDMA_IRQ` 0x04: enable bit0, FRAME_DONE_MASK bit1 (mask=1), FRAME_DONE bit16, DEADLOCK bit17, CONFIG_ERR bit21; W1C on bits 16..26.
- `DSIM_FIFOCTRL` 0x68: EMPTY_PL_SFR bit8, FULL_PL_SFR bit9, EMPTY_PH_SFR bit10, FULL_PH_SFR bit11. `DSIM_PKTHDR` = DATA1<<16 | DATA0<<8 | ID. `DSIM_CMD_CONFIG` PKT_GO_EN bit16.

Interrupts (GIC SPI number in DT, INTID = SPI + 32; DT says level-high; FD §6):

| Source | SPI | INTID |
|---|---|---|
| DECON0 extra (TE_RISE, TIME_OUT, RESOURCE_CONFLICT) | 241 | 273 |
| DECON0 frame_done | 242 | 274 |
| DECON0 frame_start | 243 | 275 |
| DPP L0 RDMA | 256 | 288 |

Timing facts:
- Link-bound frame transfer: 4 lanes x 1368 Mbps, DSC 8 bpp (2 slices of 672): `min_frame_transfer` = 5,909,637 ns (FP §3 formula, `exynos_drm_dsim.c:dsim_calc_underrun`, computed with hactive/3 = 448, vactive 2992, 24 bpp, +7 B header per line). Same at 60 and 120 Hz. So a flip at 120 Hz leaves ~2.4 ms of link idle per 8.33 ms period; DPU bandwidth per frame is unchanged from what ABL already sustains at 60 Hz.
- Panel keeps the last frame in GRAM (command mode); the DPU reads the buffer only between FRAME_START and FRAME_DONE.

Silicon facts taken as given (task statement): DECON0 idles after ABL; DPP0 RDMA reads the splash 1344x2992 BGRA8888 (RDMA format code 0) at 0xfac00000 (no DT node has this address, FD §9.1; the live value is read from RDMA at probe and used everywhere instead of a constant); SysMMU F0 is in bypass; one frame per `SHD_REG_UP_REQ` with HW trigger unmasked.

---

## 3. Frame start: TE-driven HW trigger (not SW trigger)

- Keep `TRIG_CON` as ABL/shim leave it: HW_TRIG_EN (bit0) = 1, HW_TRIG_SEL [25:24] = DDI0 (0), HW_TRIG_MASK_DECON (bit4) = 0. Probe logs it; if HW_TRIG_EN is 0 or SEL != 0, direct mode refuses to start (log `HUSKY DISP: trig %08x not TE/DDI0, staying in copy mode`).
- Why not SW trigger: a SW trigger starts the transfer immediately, so the write into GRAM races the panel's scan and tears unless the CPU times it against TE; the HW trigger does that in hardware. It is also the configuration hk3 uses (no mode sets `sw_trigger`, FP §2).
- The trigger mask (bit4) is set by this design only around panel DCS sequences (§8) and on fallback teardown; it is never toggled per frame. Per-frame masking in the Linux driver exists because it programs many registers per commit; this design writes one register (the base) before the request, and a TE that arrives between the base write and the request sends no frame (no request pending).

---

## 4. Flip mechanism

### 4.1 Per-flip register sequence (thread or ISR context, `fDispLock` held)

```
rdma[0x40/4]  = (uint32_t)pa;          // BASEADDR_Y8; pa < 4 GiB, 64-byte aligned
dsb sy
decon[0x20/4] |= 0x3;                  // GLOBAL_CON DECON_EN | DECON_EN_F (as husky_fb_kick)
decon[0x50/4]  = 0x80003FFF;           // SHD_REG_UP_REQ: GLOBAL | windows 0..13 (proven mask)
```

`IN_CTRL_0`, `SRC_SIZE`, `IMG_SIZE`, `SRC_OFFSET`, stride, window, blend, DSC: never written. A surface is scanned directly only if it matches the live RDMA geometry exactly (§5.2), so the base is the only per-frame difference.

Code comment required (AGENTS.md: undocumented behavior): the 0x80003FFF mask is the proven silicon value; the CAL's `FOR_DECON` covers only windows 0..5 (FD §2.4, §9.3).

### 4.2 Latch identification

At FRAME_START the ISR reads the RDMA shadow base (`0x19900000 + 0x400 + 0x40` = 0x19900440, FD §4.2) and compares it with ARMED.pa. Equal: ARMED becomes SCANNING. Not equal: count `latch_mismatch`, keep ARMED armed (the frame that started is someone else's request, e.g. a console kick that escaped the gate), log the first 8. This makes the bookkeeping depend on what the hardware latched, not on what was requested. Step 3 (§11) proves that the RDMA shadow follows the DECON request; if it does not (FD §9.4 is UNKNOWN), direct mode is abandoned for that boot (fallback reason `rdma-shadow`).

### 4.3 State machine (single display, two slots)

```
            SwapEnd (direct-eligible)              FRAME_START, shadow==pa
  (free) ------------------------------> ARMED ---------------------------> SCANNING
     ^   (blocks while ARMED occupied)                                         |
     |                               FRAME_DONE: retire (swap complete)        |
     +-------------------------------------------------------------------------+
```

- `fArmed`, `fScanning`: `struct husky_flip { HuskySurface *surf; uint64_t pa; uint32_t swapID; uint64_t tArm, tLatch; }` or empty.
- SwapEnd arms immediately when `fArmed` is empty, even while `fScanning` is transferring: the request is written during the transfer and latches at the first TE after FRAME_DONE (shadow registers already latched for the current frame).
- FRAME_START: `fScanning = fArmed; fArmed = empty`; wake SwapEnd waiters.
- FRAME_DONE: move `fScanning` to the retire ring (8 entries); `fScanning = empty`.
- Workloop: for each retired entry, `fRetiredSwapID = max(...)`, `IOLockWakeup(fLock, &fRetiredSwapID)`, `surf->release()` (never released in the ISR).

---

## 5. Scanout memory: contiguous pool, SysMMU stays bypassed

### 5.1 Choice and justification

Picked: a physically contiguous scanout pool in DRAM below 4 GiB, allocated once at `AppleCLCD::start`; DPU SysMMU untouched (bypass).

1. `RDMA_BASEADDR_Y8` is a single 32-bit register (`regs-dpp.h:173`, `dpp_reg.c:686`); with the SysMMU bypassed the DPU can only reach PA < 4 GiB. Ordinary IOSurfaces are scattered pages of `kernel_memory_allocate` anywhere in DRAM (FI §5.3), including the high bank at 0x880000000+. They cannot be scanned without either translation or contiguous low memory.
2. Enabling SysMMU v9 means writing a new IOMMU driver in XNU: page tables, the F0/F1 pair (`iommus = <&sysmmu_dpuf0>, <&sysmmu_dpuf1>`), VID config, TLB invalidation per map, fault IRQs (SPI 277/278), the secure base 0x19850000 and S2MPU interplay. None exists in K (FI §8). And it has to be switched on under a live DPU that is scanning the splash by PA, which faults the display at the switch unless the splash is identity-mapped first. That is a large, risky block for no frame-rate gain.
3. Cost of the pool: 4 x 16 MiB = 64 MiB of 12 GiB, plus a selection rule for which IOSurfaces get pool memory (§5.2). Surfaces outside the pool still display through the copy fallback, so a wrong selection costs performance, not correctness.
4. The GPU side already handles physical-descriptor surfaces: 0122 imports by `getPhysicalSegment`, and GPU page tables are per 4 KiB page, so contiguity is irrelevant to the GPU.

Revisit SysMMU only if CA's displayed surfaces cannot be identified at creation (step 2 shows `nonpool` swaps dominating). Parked, not part of this plan.

### 5.2 Pool and selection

- Allocation (AppleCLCD::start, `huskydisp >= 2`): `IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, slots * 16 MiB, 0x00000000FFFFC000ULL)`. Slots default 4 (`huskydisp_slots=2..6`). If the single allocation fails, try one IOBMD per slot with the same mask; keep however many succeed; 0 slots means copy mode with `HUSKY DISP: pool alloc failed (slots 0)`. Assert each slot's `getPhysicalSegment` length covers 16 MiB and end PA ≤ 0x100000000.
- Slot stride 16 MiB. Panel frame = 5376 x 2992 = 16,084,992 B, rounded to 16 KiB = 16,089,088 B.
- Surface eligibility (`husky_surface_layout` result, checked in HuskySurface create before backing allocation), all must hold:
  - creating process name is `backboardd` (CA render server; `proc_selfname`),
  - width == live RDMA width (1344) and height == live height (2992),
  - planeCount ≤ 1, bytesPerElement == 4, elementWidth == elementHeight == 1,
  - pixelFormat == 'BGRA' (0x42475241), not compressed,
  - bytesPerRow == live RDMA stride (5376), offset == 0,
  - allocSize ≤ 16 MiB, and a free slot exists.
  Ineligible or no free slot: today's `IOBufferMemoryDescriptor` path, unchanged.
- Pool surface backing: `memory = IOMemoryDescriptor::withPhysicalAddress(slotPA, allocSize, kIODirectionInOut)`, `mapCache = kIOMapDefaultCache` (cacheable; CA's CPU renderer reads its own buffers). Excluded from the wired-cap accounting (FI §5.1, it is preallocated). New field `HuskySurface::scanoutSlot` (-1 = none) and `scanoutPA`.
- Slot zeroing on assignment: `bzero` through the pool's kernel VA, then `dc civac` over the slot (reuse `husky_dcache_flush`, `bsd/dev/husky_gpu.c:512`, made non-static and declared in a header). Clean+invalidate is required, not just clean: stale dirty lines from the zeroing would otherwise be evicted later on top of GPU-written pixels.
- Slot release: `HuskySurface::free()` returns the slot. The display retains `HuskySurface` while ARMED or SCANNING, so `free()` cannot run while the DPU still references the slot.
- API between files (HuskySurface.h): `bool husky_scanout_slot_get(uint64_t size, uint64_t *pa, void **kva, int *slot)`, `void husky_scanout_slot_put(int slot)`, implemented in the new HuskyDisplay.cpp.

### 5.3 Coherency rules

- DPU reads DRAM and is treated as non-coherent (FI U10 is unknown; XNU's `clean_dcache` is a no-op under the inherited `__ARM_COHERENT_IO__`, FI §7, which is why `husky_dcache_flush` exists).
- GPU writes: the Mali CS tail flushes GPU caches and bumps the sync object, and IOGPU submit polls that sync object before returning to user space (`husky_mali_backend_bridge.c` submit path, `us < 500000`). By the time CA calls SwapEnd the pixels are in DRAM. **Precondition P1: IOGPU submission stays synchronous.** When IOGPU becomes asynchronous it must export a per-surface "last write stamp" that SwapEnd waits on before arming; until then no wait code is added (a stub would be fake success).
- CPU writes: on SwapEnd, if the surface seed (state page +0xc, bumped by `trapUnlock` for write locks, FI §5.4) differs from the seed recorded at its previous scanout, clean the slot to PoC with `dc cvac` (new `husky_dcache_clean`, same loop with `dc cvac`), then record the seed. GPU-only frames skip the clean.

---

## 6. IOMobileFramebuffer semantics

### 6.1 SwapBegin (sel 4)
Unchanged: `++fSwapID` under fLock, returned.

### 6.2 SwapEnd (sel 5)
1. Parse the 0x588 record exactly as today (offsets 0x98/0x9c/0xac/0x10c/0x14c, FI §2.4).
2. Direct-eligible iff: exactly one enabled layer; its surface is a pool surface; src rect == dst rect == (0, 0, 1344, 2992). Otherwise go to step 5.
3. Seed check + clean (§5.3).
4. `fDispLock` (spin, interrupts off). While `fArmed` is occupied: drop the spin lock, `IOLockSleepDeadline(fLock, &fArmed, now + 50 ms)`, retake it. On timeout: fallback (reason `armed-timeout`), then treat as step 5. If the same surface is currently SCANNING, count `reuse_inflight` (CA wrote into a buffer the DPU may still be reading; logged, not blocked). Then `surf->retain()`, program §4.1, set `fArmed`. Return success.
5. Copy path (not direct-eligible, or fallback mode): wait (same sleep) until both `fArmed` and `fScanning` are empty, run today's copy loop into the splash (`fPixels`, WC mapping, so no cache maintenance), then arm a flip of the splash PA with `surf = fSurface`. In fallback mode the copy is followed by the old kick sequence instead of a tracked flip.

### 6.3 SwapWait (sel 6)
- Today it is a no-op (FI §2.3). New: block until `fRetiredSwapID >= id`, or the deadline passes, or fallback is entered. Returns kIOReturnSuccess, or kIOReturnTimeout.
- Argument ABI: sel 6 is shared by `_kern_SwapWait` and `_kern_SwapWaitWithTimeout` (ios-sweet notes). **Offline pre-work O1** (before step 3): disassemble both in the iOS 26 dyld shared cache to get the scalar order. Interpretation to implement unless O1 says otherwise: `in[0]` = swap id; `in[1]`, if present, = timeout in ms; deadline = min(timeout, 100 ms); no `in[1]` means 100 ms. Step 2 logs the raw scalars of the first 32 calls to confirm.
- If `in[0] > fSwapID` (not yet begun), return kIOReturnBadArgument without sleeping.

### 6.4 Notifications
- Type 5 (vsync): one per TE edge, payload layout identical to today's (`count lo/hi, mach_absolute_time lo/hi, 0, 0`). This layout is proven with CA, so it is kept deliberately even though the ios-sweet notes describe slot 0 as a timestamp. Time = `mach_absolute_time()` sampled in the ISR at TE_RISE. If several TEs coalesce before the workloop runs, send only the latest (count stays exact). Sent with `kIOUserNotifyOptionCanDrop` as today.
- No swap-complete notification type exists in the known set (0 HotPlug, 1 Power, 3 FrameInfo, 4 CRC, 5 Vsync). Swap completion is SwapWait plus the back-pressure in SwapEnd.
- Published properties: unchanged (`MaxNumLayers = 1`). **Offline pre-work O2:** find in QuartzCore/IOMobileFramebuffer (iOS 26 DSC) the property or call CA uses for the refresh rate or interval. If a property exists, publish it with 120 after step 5 passes. If none exists, CA paces from the vsync callbacks alone.

### 6.5 Swaps in flight
- Display-owned: ≤ 2 (one ARMED, one SCANNING). CA owns every other buffer, so with ≥ 3 CA framebuffers one is always free to render into. Step 2 measures CA's framebuffer count (`distinct`).
- SwapEnd blocks only when a third direct swap arrives while ARMED is still waiting for TE, which is vsync back-pressure: at most one TE period, ~8.3 ms at 120 Hz.

---

## 7. Vsync source

- Primary: TE rising edge via `INT_PEND_EXTRA.TE_RISE` (bit16), enabled by `INT_EN_EXTRA |= 1<<16` and `INT_EN |= INT_EN_EXTRA (bit4) | INT_EN (bit0)`. The CAL never enables TE_RISE (FD §5.2); Linux uses the TE GPIO instead. So whether TE_RISE latches with `te_from = DDI0` is unproven, and step 1 measures it by polling. Code comment required (undocumented use).
- TE continues at the panel rate whether or not frames are sent (manual 120 Hz mode, §8), so vsync is continuous at 120 Hz.
- Fallback A, if TE_RISE never latches in step 1: `HW_TE_CNT` (0x38) delta. If it counts TEs, the workloop timer runs at the measured period and phase-corrects to the `HW_TE_CNT` change it sees on each tick (poll at period/4). Fallback B: the existing IOTimerEventSource at 16667 us (8333 us after a verified 120 Hz switch). The GPIO EINT path (gpp0-3) is out of scope.
- TE liveness: if no TE in 100 ms while direct mode is on, fallback reason `te-lost`, and the vsync source switches to the timer.

---

## 8. Switching the panel to 120 Hz

Source of truth: `/home/nhansp/aosp/android_kernel_google_gs-6.1_devices/google/shusky/display/panel-google-hk3.c` (HK; this is the real husky driver, FP §0; s6e3hc4 is not needed). The values below are bytes on the DSI link (facts).

### 8.1 Target state
HS op mode, manual frame mode, 120 Hz, early-exit ON (HK enables EE when vrefresh == op_hz, `HK:947-950`), fixed TE at the peak rate, HBM off. IRC and TE2 untouched (no change in those features; TE2 has no host wire, FP §13.3). Deliberate divergence from HK, to be commented in code: HK re-sends IRC/TE2 blocks only on change, and we never change them.

### 8.2 DCS sequence 60 -> 120 (one atomic batch; HK comment at `HK:669-673`: send as a whole)
Built per `hk3_set_panel_feat` (`HK:542-875`) for feat = {EE}, !HBM, !NS, manual, vrefresh 120, `force_changeable_te` false:

```
F0 5A 5A                                   unlock
B9 51                                      fixed TE
B0 00 02 B9
B9 00                                      TE freq = peak (120)
B0 00 08 B9
B9 0B BB 00 2F 0B BB 00 2F                 fixed TE width, HS
F2 01                                      op mode set
60 00                                      HS
BD 21 01 83 03 03                          early exit on, no HBM
B0 00 10 BD
BD 22
B0 00 82 BD
BD 22 22 22 22
B0 00 1E BD
BD 00 00 00 02 00 06 00 16 00 2E 00 EE     HS, non-HBM table
BD 21                                      manual
60 00                                      120 Hz (HS)
F7 0F                                      freq_update
F0 A5 A5                                   lock
```

The `F2 01 / 60 00` op-mode pair is always sent because the ABL op mode is unknown (FP §13.7). It is idempotent when ABL was already HS.

### 8.3 DCS sequence 120 -> 60 (revert; HS op kept, EE off, changeable TE)

```
F0 5A 5A
B9 04
B0 00 04 B9
B9 0B BB 00 2F
BD 21 81 83 03 03
B0 00 10 BD
BD 00
B0 00 82 BD
BD 00 00 00 00
B0 00 1E BD
BD 00 00 00 02 00 06 00 16 00 2E 00 EE
BD 21
60 01
F7 0F
F0 A5 A5
```

### 8.4 DSIM packetization (new minimal TX in HuskyDisplay.cpp, polled)
- Data type by length (MIPI DCS): 1 byte -> 0x05 (DATA0 = cmd, DATA1 = 0); 2 bytes -> 0x15 (DATA0 = cmd, DATA1 = param); ≥ 3 bytes -> 0x39 long (DATA0 = len & 0xff, DATA1 = len >> 8). VC 0, so ID = DT.
- Long packet: write the payload to `DSIM_PAYLOAD` as little-endian 32-bit words (last word zero-padded), then the header to `DSIM_PKTHDR`. Short packet: header only.
- Whole batch: 19 packets, ~110 payload bytes, within PH FIFO 32 / PL FIFO 2048 (FP §5). Before each header, check FULL_PH_SFR == 0 and FULL_PL_SFR == 0 (if full: poll up to 5 ms, then fail).
- Completion: poll `FIFOCTRL` until EMPTY_PH_SFR and EMPTY_PL_SFR are both set, timeout 50 ms (3 frames at 60 Hz; commands may be held by the DSIM TE-protect window, FP §5).
- If `DSIM_CMD_CONFIG.PKT_GO_EN` is set at probe, clear it before sending (packet-go would hold packets until PKT_GO_RDY). Comment as a deliberate divergence. `ESCMODE.CMD_LPDT` is left as ABL set it (HK sends HS); probe logs it.
- Context: thread (workloop), never ISR. `fDispLock` is not held while polling; the engine is quiesced instead (§8.5).

### 8.5 Switch procedure (`huskydisp >= 5`, on the workloop, before the first direct flip)
1. Read the TE rate (§11 step 1 method, 250 ms). If 115-125 Hz, skip to 5 (ABL already at 120).
2. Quiesce: wait until `fArmed` and `fScanning` are empty (≤ 50 ms); set `TRIG_CON |= HW_TRIG_MASK_DECON` (bit4).
3. Send §8.2. On TX timeout: clear bit4, fail (reason `dcs-timeout`), stay 60 Hz, no revert (nothing was latched).
4. DSIM timers for 120 fps (values from `dsim_calc_underrun` and `dsim_reg_get_cmd_timer` with hk3 `te_idle_us = 350`, `te_var = 1`, `HK:2272-2275`; computed in this design):

   | reg | 60 Hz value | 120 Hz value |
   |---|---|---|
   | UNDERRUN_CTRL 0x34 [15:0] | 0x2235 (8757) | 0x06A7 (1703) |
   | CMD_TE_CTRL1 0x88 | 0x34E13D3B (protect 13537, tout 15675) | 0x1A701E9D (6768, 7837) |
   | CMD_TE_CTRL0 0x84 | 26 | 26 (unchanged) |

   Guard: write the 120 values only if the probed values equal the 60 Hz column, or already equal the 120 column. Any other value means ABL used inputs this design does not know, so revert with §8.3 and abort (reason `dsim-timers`). Android updates these registers on a refresh change through `dsim_reg_set_rr_config` (`dsim_reg.c:2177-2188`).
5. Wait 2 TE edges, clear bit4, request one frame of the current buffer (the splash or the last swap): §4.1 with that PA.
6. Measure TE for 250 ms. Pass: 28-32 edges, then `HUSKY DISP: panel 120Hz ok period %u us`, the vsync timer fallback period becomes 8333, publish the O2 property if found. Fail: quiesce, send §8.3, restore the 60 Hz DSIM values, request a frame, `HUSKY DISP: panel 120Hz FAILED (te %u/250ms), reverted to 60`.
- `huskydisp_hz=60` skips the switch at any N ≥ 5.

---

## 9. Locking and IRQ context

### 9.1 Locks

| Lock | Type | Protects | Taken from |
|---|---|---|---|
| `fDispLock` | `lck_spin_t`; thread side takes it with interrupts disabled (`ml_set_interrupts_enabled(FALSE)`) | `fArmed`, `fScanning`, retire ring, TE count/time, error flags, all DECON/RDMA MMIO writes | ISR (CPU0), SwapEnd, workloop, watchdog |
| `fLock` (existing IOLock) | sleeping mutex | clients, ports, callbacks, `fRetiredSwapID`, the sleep/wakeup events `&fArmed` and `&fRetiredSwapID` | threads only, never ISR |
| slot bitmap | `lck_spin_t` (`husky_scanout_lock`) | pool slot allocation | surface create/free |

Order: `fLock` before `fDispLock`. Never sleep, allocate, or send Mach messages while holding `fDispLock`.

### 9.2 IRQ plumbing (new)
- `pe_fiq.c`: `void husky_gic_spi_enable(uint32_t intid, uint8_t prio)`, which writes `GICD_ICFGR` (level: 2-bit field = 0b00), `GICD_IPRIORITYR` byte = 0xA0, `GICD_IROUTER(intid)` = boot CPU affinity (MPIDR Aff3..Aff0 saved in `pe_init_fiq` on the boot CPU; Interrupt_Routing_Mode = 0), then `GICD_ISENABLER`, then waits for `GICD_CTLR.RWP` to clear. `IGROUPR`/`IGRPMODR` are not written: Non-secure writes are ignored (pe_fiq.c comment). Probe reads `GICD_IGROUPR` and requires bit = 1 (Group 1 NS) for each INTID, else no IRQ mode (reason `gic-group`).
- `sleh.c` HUSKY branch: before the `cdp->interrupt_handler` fallback, look up `husky_irq_table[intid - 32]` (320 entries, INTID 32..351) and call `fn(arg, intid)` if registered; EOIR as today. Registration `husky_irq_register(intid, fn, arg)` only from thread context before `husky_gic_spi_enable`.
- One handler `husky_disp_irq(arg, intid)`:
  - 275 FRAME_START: W1C `INT_PEND` bit12; latch check (§4.2); timestamp.
  - 274 FRAME_DONE: W1C bit13; SCANNING -> retire ring.
  - 273 EXTRA: read `INT_PEND_EXTRA`, W1C the read bits; TE_RISE -> `teCount++`, `teTime = mach_absolute_time()`; TIME_OUT or RESOURCE_CONFLICT -> `errFlags`.
  - 288 RDMA: read `RDMA_IRQ`, W1C bits 16..26; CONFIG_ERR or DEADLOCK -> `errFlags`. The RDMA FRAME_DONE source is masked (`RDMA_IRQ` bit1 = 1) at enable.
  - Then `fEventSource->signalWorkAvailable()` (IOWorkLoop's work lock is interrupt-safe).
  - Level-triggered: the device W1C happens before EOIR (sleh EOIs after the handler returns).
- `HuskyDispEventSource : IOEventSource` on AppleCLCD's workloop. `checkForWork()` snapshots state under `fDispLock`, then without it: sends vsync, retires swaps (wake `&fRetiredSwapID`, release surfaces), wakes `&fArmed` waiters after a latch, and handles `errFlags` (§10).
- Watchdog `IOTimerEventSource` every 20 ms (also the polled engine in step 3): latch timeout, transfer timeout, TE lost.

### 9.3 Console kick gate
- Global `volatile int husky_disp_direct` and `uint32_t husky_disp_splash_pa`, plus a pre-mapped `volatile uint32_t *husky_rdma0` set by AppleCLCD before direct mode starts.
- `husky_fb_kick()` (pe_serial.c:897): if `husky_disp_direct && !panic_active()`, return. If `husky_disp_direct && panic_active()`, write `husky_rdma0[0x40/4] = husky_disp_splash_pa` (once) before the existing kick, so panic text drawn in the splash is shown. Never map in the panic path (hence the pre-map).

---

## 10. Failure handling

| Detector | Threshold | Action |
|---|---|---|
| Latch timeout (ARMED age) | > 3 TE periods + 2 ms | once: re-issue §4.1 for the same PA; second consecutive: fallback `latch-timeout` |
| Transfer timeout (SCANNING age) | > 20 ms | fallback `xfer-timeout` |
| Latch mismatch | > 8 in a row | fallback `latch-mismatch` |
| DECON EXTRA TIME_OUT / RESOURCE_CONFLICT | any | fallback `decon-err %x` |
| RDMA CONFIG_ERR / DEADLOCK | any | fallback `rdma-err %x` |
| TE lost | no TE for 100 ms | fallback `te-lost`, vsync from timer |
| SwapEnd armed wait | > 50 ms | fallback `armed-timeout` |
| Probe preconditions (trig, SysMMU bit0 set, RDMA fmt != 0, geometry mismatch, splash PA ≥ 4 GiB, GIC group) | at start | stay in copy mode, log the reason |

Fallback (sticky for the boot, no automatic re-entry, so the display state is deterministic and cannot flap):
1. Under `fDispLock`: mask the DECON IRQ sources (`INT_EN` = 0 except EXTRA if TE vsync still works), move ARMED and SCANNING to the retire ring, set `fMode = COPY`.
2. Write `BASEADDR_Y8 = splash PA` and request one frame (§4.1). The splash still holds the last copied or console frame.
3. `husky_disp_direct = 0` (the console kick works again).
4. Workloop: retire all (wake SwapWait/SwapEnd waiters), release surfaces, choose the vsync source (TE if alive, else the timer at the current panel period).
5. Log `HUSKY DISP: fallback (%s) -> copy mode`. From then on SwapEnd uses today's copy loop into the splash plus the kick, exactly the pre-design behavior. Pool surfaces remain valid memory, so CA keeps working.

Panel 120 Hz failure is handled in §8.5 (revert to 60 Hz). It does not force copy mode.

---

## 11. Bring-up plan (`huskydisp=N`, cumulative, one boot per step)

All lines are `IOLog`/`kprintf` with prefix `HUSKY DISP:`, so they reach the pmsg persistent log. Combine with the usual `huskyreboot=N` for unattended reboots. Each step's code stays in the tree and runs at higher N.

### Step 0: prerequisites (no boot)
- Apply 0121/0122 to K (P0). Offline O1 (SwapWait ABI), O2 (refresh-rate property), O3 (log the CA framebuffer creation property keys if the DSC shows a distinguishing key such as a name; optional, step 2 logs it live anyway).

### Step 1: `huskydisp=1`, read-only probe and TE rate
Code: HuskyDisplay.cpp maps the blocks, dumps them, measures TE. The only write is `INT_EN_EXTRA |= TE_RISE` if the first measurement sees no edges (no GIC enable, so no CPU IRQ). No behavior change otherwise.
Log lines and pass criteria:
```
HUSKY DISP: probe decon gcon=%08x trig=%08x shd=%08x en=%08x enx=%08x pend=%08x pendx=%08x tecnt=%u
    pass: gcon bit8 (CMD) = 1; trig bit0 = 1, bit4 = 0, [25:24] = 0; shd = 0
HUSKY DISP: probe win%u con=%08x start=%08x end=%08x        (each window with WIN_EN)
    pass: one window with CHMAP [6:4] = 0, start=00000000, end=0baf053f
HUSKY DISP: probe rdma0 en=%08x ctrl0=%08x src=%08x off=%08x img=%08x base=%08x sbase=%08x st0=%08x st1=%08x
    pass: ctrl0[13:8] = 0; src = 0bb00540; img = 0bb00540; off = 0; base = sbase = splash PA (expect fac00000)
HUSKY DISP: probe dsim cfg=%08x esc=%08x urun=%08x te0=%08x te1=%08x cmdcfg=%08x fifo=%08x pll=%08x
    pass: cfg bit18 = 0 (command mode); urun/te1 equal one column of the §8.5 table (records ABL's rate)
HUSKY DISP: probe sysmmu ctrl=%08x                           pass: bit0 = 0
HUSKY DISP: probe gic %u grp=%u cfg=%u en=%u route=%016llx   (273, 274, 275, 288)   pass: grp=1
HUSKY DISP: probe dram base=%llx size=%llx splash=%llx in_dram=%d
    record: whether the low bank (< 4 GiB) is XNU-managed; whether the splash lies inside it (must be 0)
HUSKY DISP: probe te pend=%u hwcnt=%u->%u in 500 ms (enx_forced=%d)
    pass: pend 29-31 (60 Hz) or 59-61 (120 Hz); record whether HW_TE_CNT counts TEs
```
If `in_dram=1` for the splash, stop: XNU can hand out the splash pages. Fix the shim memory map first.

### Step 2: `huskydisp=2`, scanout pool, still copy path
Code: pool allocation, surface eligibility, slot zeroing, SwapWait argument logging, swap statistics. SwapEnd still copies (from pool memory now).
```
HUSKY DISP: pool %u slots pa=%llx-%llx kva=%p           pass: slots = 4, end ≤ 100000000
HUSKY DISP: slot %u -> sid %u %ux%u rb %u fmt %08x by %s keys=[...]   (every pool assignment; keys = property dict keys, name if present)
HUSKY DISP: nopool sid %u %ux%u by %s (%s)              (panel-sized surfaces refused, with reason; first 16)
HUSKY DISP: swapwait in=%u [%llx %llx %llx %llx]        (first 32)
HUSKY DISP: swap stats n=%u pool=%u nonpool=%u distinct=%u   (at n = 300 and every 3000)
    pass: nonpool = 0 after the first few swaps; record distinct (CA framebuffer count, expect ≥ 3)
```
Visual: UI identical to before.

### Step 3: `huskydisp=3`, direct flips, polled (no GIC yet)
Code: §4 and §6 complete, driven by the 20 ms watchdog plus a 250 us poll loop while anything is in flight (reads `INT_PEND` and calls the same service routine as the future ISR). Vsync is still the 60 Hz timer. The console gate is on.
```
HUSKY DISP: direct on splash=%08x mask=80003fff
HUSKY DISP: flip #%u sid %u pa %08x latch %u us xfer %u us   (first 32, then every 512th)
    pass: sbase == pa at every latch; latch ≤ 17000 us; xfer 5500-6500 us (poll granularity +250 us)
HUSKY DISP: stats flips=%u copies=%u mism=%u reuse=%u drops=%u err=%x   (every 10 s)
    pass: copies = 0 after the first second; mism = 0; err = 0; reuse = 0 (else CA has too few buffers)
```
Visual: UI animates, no tearing. Fail signature: `fallback (rdma-shadow)`, meaning RDMA does not latch via the DECON request. Capture C6, then stop.

### Step 4: `huskydisp=4`, IRQs and TE vsync, still 60 Hz
Code: §9.2 (GIC helper, sleh table, ISR, event source). The poll loop is removed from the hot path (the watchdog remains). Vsync comes from TE_RISE.
```
HUSKY DISP: irq %u on (prio a0 route %016llx)          (x4)
HUSKY DISP: vsync src=te period %u us                  pass: 16600-16720
HUSKY DISP: stats te=%u fs=%u fd=%u flips=%u drops=%u err=%x   (every 10 s)
    pass: te ≈ 600 per 10 s; fs = fd = flips; err = 0; no "husky: unhandled IRQ" lines
```
If TE_RISE was dead in step 1: expect `vsync src=hwcnt` or `src=timer` and the same stats without te.

### Step 5: `huskydisp=5`, panel at 120 Hz
Code: §8.
```
HUSKY DISP: panel te %u Hz before switch
HUSKY DISP: dcs %u pkts drained in %u us                pass: 19 pkts, < 50000 us
HUSKY DISP: dsim urun %04x->%04x te1 %08x->%08x         pass: 2235->06a7, 34e13d3b->1a701e9d
HUSKY DISP: panel 120Hz ok period %u us                 pass: 8300-8370
HUSKY DISP: vsync src=te period %u us                   pass: 8300-8370
HUSKY DISP: flip #... latch %u us xfer %u us            pass: latch ≤ 8400, xfer 5800-6100
HUSKY DISP: stats te=%u ...                             pass: te ≈ 1200 per 10 s, err = 0
```
Visual: smooth scrolling. DSIM underrun check: `stats` also prints the `DSIM_INTSRC` UNDER_RUN (bit19) count; it must be 0. If it is not 0, raise MIF/INT through the existing DVFS boot-args (memory: husky-boot-cycle-tools) and repeat.

### Step 6: `huskydisp=6`, failure path (`huskydisp_inject=1|2`)
- `inject=1`: skip one SHD request at flip 100. Expect `latch-timeout retry` then normal flips (no fallback).
- `inject=2`: set a fake `rdma-err` at flip 200. Expect `HUSKY DISP: fallback (rdma-err inject) -> copy mode`, then `stats copies` rising, UI still updating, vsync still from TE at 120 Hz.
- Panic visibility is checked opportunistically on the next real panic (splash shows panic text).

Squeeze rule (memory: squeeze-boot-cycles): steps 1+2 can share one boot (2 includes 1's probe output). Steps 3 and 4 must be separate boots, because step 3 isolates DPU semantics from GIC work.

---

## 12. Live-capture list (main session)

From XNU boots (pmsg logs, steps above):
- C1 (step 1): DECON/RDMA/DSIM/SysMMU/GIC probe lines; ABL refresh rate (TE count and DSIM timer column); whether TE_RISE and HW_TE_CNT work; splash PA; XNU DRAM range versus splash and low bank.
- C2 (step 2): CA framebuffer count, creating process, property keys; SwapWait scalars.

From Android on the phone (root via KernelSU, screen ON; DPU registers are only safe to read while the display is powered; never read clk debugfs, memory: no-clk-debugfs):
- C3: DSIM0 `0x19440034`, `0x19440084`, `0x19440088` at 120 Hz and at 60 Hz (switch with `settings put system peak_refresh_rate 60` / `min_refresh_rate`). Confirms the §8.5 table, or replaces it with measured values.
- C4: `echo 'func hk3_set_panel_feat +p' > /sys/kernel/debug/dynamic_debug/control`, then toggle 60/120 and capture the dmesg `op=.. ee=.. hbm=.. fi=.. fps=..` lines. Confirms 120 Hz = hs / ee on / manual / fps 120 on this device (`force_changeable_te` off).
- C5: DECON0 `0x19470030` (TRIG_CON) and `0x19470064` (INT_EN_EXTRA) under Android at 120 Hz: confirms the HW trigger config and whether Android ever enables TE_RISE.
- C6 (only if step 3 fails with `rdma-shadow`): RDMA0 `0x19900040` and `0x19900440` sampled repeatedly during a UI animation under Android, to see when the shadow follows the active register.
- C7 (optional, only if IRC/TE2 changes are ever needed): panel rev/material from dmesg (`hk3_get_panel_rev`).

---

## 13. File plan (K)

| File | Change |
|---|---|
| `iokit/Kernel/arm/HuskyDisplay.cpp/.h` (new) | MMIO maps, probe, flip engine (§4), ISR + event source (§9), DSIM TX + 120 Hz switch (§8), scanout pool (§5), failure handling (§10). Listed in `iokit/conf/files.arm64` next to HuskyFramebuffer.cpp. |
| `iokit/Kernel/arm/HuskyFramebuffer.cpp` | SwapEnd split (direct/copy), SwapWait, vsync source selection, `huskydisp` parsing; the header comment is corrected (1344x2992 BGRA, 0x588 record offsets, vsync source). |
| `iokit/Kernel/arm/HuskySurface.cpp/.h` | eligibility rule, pool backing, `scanoutSlot/scanoutPA`, slot release in `free()`, step-2 logging. |
| `pexpert/arm/pe_fiq.c` | `husky_gic_spi_enable`, boot-CPU affinity capture. |
| `osfmk/arm64/sleh.c` | per-INTID dispatch table in the HUSKY branch. |
| `pexpert/arm/pe_serial.c` | kick gate and panic splash restore (§9.3). |
| `bsd/dev/husky_gpu.c` | export `husky_dcache_flush`; add `husky_dcache_clean` (`dc cvac`). |

Comments in code only where AGENTS.md allows them: the 0x80003FFF mask, TE_RISE use, RDMA shadow read for latch identification, PKT_GO clear, the coherency clean+invalidate after zeroing, and the skipped IRC/TE2 blocks.
