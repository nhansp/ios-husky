# Husky display: implementation of DESIGN.md (kernel side)

Tree: `/home/nhansp/aosp/ios-husky/out/xnu-display-wt` (K), `base 0001-0120` + P0 + this change, uncommitted.
Nothing was built or run. Each changed or new translation unit was syntax-checked with host
clang 18 (`-fsyntax-only`) using the dxb compile flags and generated headers from
`xnu-buildtree/buildtree.tgz` (Apple typed-allocator builtins stubbed): `HuskyDisplay.cpp`,
`HuskySurface.cpp`, `HuskyFramebuffer.cpp`, `IOGPUMemory.cpp`, `pe_fiq.c`, `pe_serial.c`,
`husky_gpu.c` compile with 0 errors and no warnings from the new lines (shorten-64-to-32 and
implicit-int-conversion enabled). `sleh.c` could not be checked on the host (`__builtin_clzg`,
`TrustCache/API.h` missing); its change is 33 lines of plain C.
Review fixes (ISR pending-bit dispatch with readback; `HW_TE_CNT` as a 16-bit field chosen at probe) re-checked the same way:
`HuskyDisplay.cpp` shows only the typed-allocator stub errors at the two `OSDefineMetaClassAndStructors` lines, no diagnostics from the changed code.

**cal_9865 rework (2026-10-27).** The first version was written against `cal_9845` (gs101); zuma builds `cal_9865`
(`display/samsung/Kbuild:30-38`). Registers, bitfields, sequences, the probe verdict and the TE/vsync path were
re-derived from `cal_9865` (section 2a), from `fix-decon.md`, `fix-dpp.md`, `fix-dsim.md`, each item re-read in the CAL
sources. `HuskyDisplay.cpp` was re-syntax-checked the same way (clang 18, the dxb flags, `-Wall -Wextra
-Wshorten-64-to-32 -Wimplicit-int-conversion`): only the two stub errors. Not built, not run on silicon. The probe
verdict was checked by hand against the boot 0127d2 readings (section 5, step 1).

## 1. Files changed

| File | Change |
|---|---|
| `iokit/Kernel/arm/HuskyDisplay.h` (new) | Engine class, `HuskyDisplayOps` callbacks into AppleCLCD, `husky_flip`, `HUSKY_DISP_LOG` (printf + kprintf, the husky `HU_LOG` pair). |
| `iokit/Kernel/arm/HuskyDisplay.cpp` (new) | cal_9865 register maps, probe, `HW_TE_CNT` measurement, scanout pool, flip engine, ISR + work-loop event source, watchdog/poll timers, DSIM TX, 60 to 120 Hz switch, fallback, stats. |
| `iokit/Kernel/arm/HuskyFramebuffer.cpp` | Header comment corrected (1344x2992 BGRA, 0x588 record offsets, vsync source). `AppleCLCD`: `fDisp`, `sendVsync`, `dispVsync`/`dispTimer` callbacks, `swapDirect`, copy path wrapped in `copyBegin`/`copyEnd`, SwapWait (sel 6) forwarded. |
| `iokit/Kernel/arm/HuskySurface.h/.cpp` | `scanoutSlot/PA/KVA/Seed`, `seed()`, pool eligibility and backing at create, slot release in `free()`, `kernelAddress()` returns the pool KVA, step-2 `slot`/`nopool` logs, `husky_scanout_*` API declarations. |
| `pexpert/arm/pe_fiq.c` | Boot-CPU affinity capture, `husky_gic_spi_enable(intid, prio)`, `husky_gic_spi_probe(...)`. |
| `osfmk/arm64/sleh.c` | `husky_irq_table[320]` (INTID 32..351), `husky_irq_register`, dispatch before the `cdp->interrupt_handler` fallback in the HUSKY branch of `sleh_irq`. |
| `pexpert/arm/pe_serial.c` | `husky_disp_direct`, `husky_disp_splash_pa`, `husky_rdma0`; `husky_fb_kick` gate and panic splash restore. |
| `bsd/dev/husky_gpu.c` | `husky_dcache_clean` (`dc cvac`); prototypes for it and `husky_dcache_flush` (already non-static). |
| `iokit/conf/files.arm64` | `HuskyDisplay.cpp` after `HuskyFramebuffer.cpp`. |
| P0 (applied, not mine) | `git diff 1a76375 0d058f1` of the artifacts repo (0121 + 0122) applied to K: `husky_gpu.c`, `IOGPUCommandQueue.cpp`, `IOGPUMemory.cpp/.h`, `husky_mali_backend_bridge.c`. Artifacts `HEAD` also has 0123 (bridge only); it is not applied because DESIGN P0 names 0121/0122 only. |

Default boot: with `huskydisp` absent or 0, `HuskyDisplay::create` returns NULL, so `fDisp == NULL`
everywhere: the copy loop, the 16667 us vsync timer, the no-op SwapWait, the console kick, the
buffer-backed IOSurfaces and the IRQ table (empty) behave as before.

## 2. Boot-args

| Arg | Meaning |
|---|---|
| `huskydisp=N` | Cumulative step, 0 or absent = off (section 4). |
| `huskydisp_slots=2..6` | Pool slots of 16 MiB (default 4). |
| `huskydisp_hz=60` | Never switch the panel to 120 Hz (default tries at N >= 5). |
| `huskydisp_inject=1\|2` | N >= 6 only. 1: skip one SHD request at flip 100. 2: fake RDMA error at flip 200. |
| `huskydisp_cmp=1` | Diagnostic. Adds `SHD_REG_UP_REQ_CMP` (bit 20) to every frame request, as `decon_reg_update_req_global` does on DECON0. Default off: the console's `0x80003FFF` moves frames without it. |
| `huskydisp_phy=1` | Diagnostic. Maps `0x19460000` and logs the DSIM0 PHY PLL (`probe dphy pll ...`). Default off: the PHY bank has not been read before. |
| `huskydisp_terise=1` | Diagnostic. Retests `INT_PEND_EXTRA.TE_RISE` for 500 ms with the zuma enable sequence and restores `INT_EN`/`INT_EN_EXTRA` (`probe te_rise pend=N`). Logged only; TE comes from `HW_TE_CNT` regardless. |

## 2a. Register map (cal_9865)

Names and offsets from `display/samsung/cal_9865/regs-{decon,dpp,dsim}.h`, bases from `zuma-drm-dpu.dtsi`. "Was" is the cal_9845 value the first version used.

**DECON0 main, `0x19470000`**

| Register | Offset | Fields used | Use | Was / change |
|---|---|---|---|---|
| `GLOBAL_CON` | 0x20 | `DECON_EN_F` [0], `DECON_EN` [1], `RUN_STATUS` [4], `OPERATION_MODE_CMD` [8] | `\|= 3` per flip; RUN_STATUS polled (2 ms) before a trigger unmask; CMD checked by the probe | RUN_STATUS wait new (`decon_reg_start`) |
| `TRIG_CON` | 0x30 | `HW_TRIG_EN` [0], `HW_TRIG_MASK_DECON` [4], `HW_TRIG_SEL` [25:24] | mask = MASK 1 + EN 0, unmask = EN 1 + MASK 0 (`decon_reg_set_trigger`); the other bits stay as read: [6:5] have no 9865 name, [13:12] `ACTIVE_VALUE`/`EDGE_POLARITY` are never written by the zuma driver | was `\|= MASK` / `&= ~MASK` with EN left set |
| `HW_TE_CNT` | 0x38 | `HW_TRIG_CNT_A` [15:0], `HW_TRIG_CNT_B` [31:16] | the TE edge source for probe, vsync, TE-lost watchdog | now the only source |
| `SHD_REG_UP_REQ` | 0x50 | `GLOBAL` [31], `FOR_DECON` [13:0] = 0x3fff (14 windows), `CMP` [20] | request `0x80003FFF`; `CMP` only with `huskydisp_cmp=1`; quiesce waits for 0 | `FOR_DECON` 0x3F was gs101; value unchanged, comment corrected |
| `DECON_INT_EN` | 0x60 | master [0], `EXTRA` [4], `FRAME_START` [12], `FRAME_DONE` [13] | set by `enableDecon`; fallback clears master and `EXTRA` only (`decon_reg_set_interrupts(0)`) | fallback cleared the whole register; DQE dimming [21:20] not enabled (no DQE) |
| `DECON_INT_EN_EXTRA` | 0x64 | `TIME_OUT` [0], `RESOURCE_CONFLICT` [4] | stays enabled, never cleared; `TE_RISE` [16] is no longer enabled by the engine | was forcing `TE_RISE`, clearing the register in fallback |
| `DECON_INT_PEND` / `_EXTRA` | 0x70 / 0x74 | as above, W1C | unchanged | none |
| `RSC_STATUS_0` / `_2`, `CLOCK_CON(0)` | 0x100 / 0x108, 0x5000 | channel and window owner, `AUTO_CG_EN_*` | read-only probe line | new |

**DECON0 windows, `0x19480000` (`DECON_WIN`) and `0x194A0000` (`DECON0_WINCON`), stride 0x1000**

| Register | Offset | Fields | Use |
|---|---|---|---|
| `DECON_CON_WIN(n)` | +0x0 (wincon) | `WIN_EN_F` [0], `WIN_CHMAP` [7:4] | verdict: exactly one window enabled and its channel is 0 (DPP0) |
| `WIN_START_POSITION` / `WIN_END_POSITION` | +0xC / +0x10 (win) | X [13:0], Y [29:16] | verdict: (0,0) to (width-1, height-1) |

**DPU_DMA L0 (RDMA), `0x19900000`**

| Register | Offset | Fields | Use | Was |
|---|---|---|---|---|
| `RDMA_IRQ` | 0x04 | enable [0], masks [12:1], pending [27:16] | `IDMA_ALL_IRQ_MASK` = `0xDFB << 1` (bits 1,2,4-9,11,12), `IDMA_ALL_IRQ_CLEAR` = `0xDFB << 16` (16,17,19-24,26,27); frame-done stays masked | `0x7FB << 1`, `0x7FF << 16` (bits 10, 18, 25 do not exist; bits 12, 27 VOTF were missing) |
| `RDMA_IN_CTRL_0` | 0x08 | `IMG_FORMAT` [13:8], `ROT` [6:4], `AFBC` [3], `SBWC` [2], `SAJC` [1], `BLOCK` [0], `SBWC_LOSSY` [14] | verdict: format 0..7 (linear 32-bit RGB), no rotation/compression/block | required format 0 |
| `RDMA_SRC_WIDTH` | 0x10 | [23:0] | verdict | was `SRC_SIZE` with the height in [31:16] |
| `RDMA_SRC_HEIGHT` | 0x14 | [23:0] | verdict | was read as `SRC_OFFSET` |
| `RDMA_SRC_OFFSET` | 0x18 | X [15:0], Y [31:16], must be 0 | verdict | was read as `IMG_SIZE` |
| `RDMA_IMG_SIZE` | 0x1C | width [15:0], height [31:16] | verdict: equals (width, height) | was 0x18 with 14-bit fields |
| `RDMA_BASEADDR_P0` | 0x40 | 32-bit | the flip; also written by `husky_fb_kick` | was named `BASEADDR_Y8`; same offset |
| `RDMA_SRC_STRIDE_0/1` | 0x50 / 0x54 | | logged; the zuma driver programs them only for SBWC | unchanged |
| shadow bank | +0x400 | `BASEADDR_P0` at 0x440 | latch check | unchanged |

Error pending bits acted on: `DEADLOCK` 17, `READ_SLAVE_ERR` 19, `CONFIG_ERR` 21, `FBC_ERR` 23 (the driver's `dma_irq_handler` set); was 17 and 21.

**DSIM0, `0x19440000`**

| Register | Offset | Fields | Use | Was / change |
|---|---|---|---|---|
| `DSIM_UNDERRUN_CTRL` | 0x34 | `CM_UNDERRUN_LP_REF` [15:0] | 60 Hz `0x2235` (ABL's, equal to `dsim_calc_underrun`), 120 Hz `0x06A7` | unchanged |
| `DSIM_CMD_TE_CTRL1` | 0x88 | `TIME_TE_PROTECT_ON` [31:16], `TIME_TE_TOUT` [15:0] | 120 Hz value `hd_te1(120)` = `0x1A701E9D` (`dsim_reg_get_cmd_timer`: `hs*(100-5)*100/fps/16`, `hs*(100+10)*100/fps/16`, hs 1368); the probed 60 Hz value is restored on revert | was the constants `0x34E13D3B`/`0x1A701E9D`, and the gate required the 60 Hz one; ABL's is `0x34C53C1E` |
| `DSIM_CONFIG`, `ESCMODE`, `INTSRC`, `PKTHDR`, `PAYLOAD`, `FIFOCTRL`, `CMD_CONFIG`, `CMD_TE_CTRL0` | 0x4C, 0x2C, 0x50, 0x58, 0x5C, 0x68, 0x80, 0x84 | as in `regs-dsim.h` | unchanged | none |
| `DSIM_LINK_STATUS3` | 0x14 | `PLL_STABLE` [0] | probe line | replaces the 0xA0 read |
| `DSIM_SFR_CTRL` | 0x64 | `SHADOW_EN` [0], `SHADOW_REG_READ_EN` [1] | probe line, and the 120 Hz timer readback: with `SHADOW_EN` set and `READ_EN` clear a read returns the operating bank (the write only lands on the next frame, the trigger is masked), so `dsimTimersRead` sets `READ_EN` around the readback and restores the live value after; logged as `sfr=` on the `dsim urun` line | new |
| `DSIM_CPRS_CTRL`, `SLICE01`, `SLICE23` | 0x74, 0x78, 0x7C | DSC state | probe line | new |
| `DSIM_PLL_CTRL` | 0xA0 | | removed: not a cal_9865 register, the 0 read carried no PLL information | was probed |

**DSIM0 PHY PLL, `0x19460100` (`huskydisp_phy=1`):** `PLL_CON0` +0x00 (`PLL_EN` [12], `PMS_S` [10:8], `PMS_P` [5:0]), `PLL_CON1` +0x04 (`PMS_K`), `PLL_CON2` +0x08 (`PMS_M` [9:0]), `PLL_STAT0` +0x40 (`LOCK` [0]). Expected for hs 1368: en 1, p 1, m 111, s 2, k 0x5400, lock 1.

**Unchanged, verified:** DECON/RDMA bases and INTIDs (273/274/275/288), SysMMU v9 `CTRL_VM` 0x8000 bit 0 (`__sysmmu_enable` sets it in every VM, so VM 0 stands for the device), window bank layout.

## 3. How it works (code map)

- **Modes** (`fMode`): `Copy` (today), `Bringup` (engine thread owns DECON0: console gate on, swaps copy into the splash silently), `Direct`, `Fallback` (sticky).
- **Flip** (`programFlip`): `RDMA_BASEADDR_P0 = pa; dsb sy; GLOBAL_CON |= 3; SHD_REG_UP_REQ = 0x80003FFF` (plus `CMP` with `huskydisp_cmp=1`). Nothing else is written per frame. `arm()` fills the ARMED slot; `onFrameStart` reads the RDMA shadow base (+0x440) and promotes ARMED to SCANNING only if it equals the armed PA; `onFrameDone` moves SCANNING to the retire ring (8). `doWork` (work loop) logs, releases surfaces, wakes SwapEnd/SwapWait waiters, sends vsync, applies errors.
- **SwapEnd** (`AppleCLCD::swap`): exactly one enabled layer, pool surface, src and dst rect whole-panel (a zero w/h means whole surface, as the copy loop treats it) -> `swapDirect` (seed check + `dc cvac` if CPU-written, then `armWait`, which blocks at most 50 ms while ARMED is occupied, else fallback `armed-timeout`). Otherwise `copyBegin` (wait for both slots empty), the unchanged copy loop, `copyEnd` (arm a flip of the splash; in Fallback `husky_fb_kick`; in Bringup just retire the swap id).
- **SwapWait**: blocks until `fRetiredSwapID >= in[0]`, deadline `min(in[1], 100 ms)` (100 ms without `in[1]`), `BadArgument` if the id was never begun, `Timeout` on expiry. First 32 calls log their raw scalars. Only blocks in Direct mode.
- **Pool** (`allocPool`): one contiguous `inTaskWithPhysicalMask(0xFFFFC000)` of `slots x 16 MiB`, else one per slot; every slot checked to be below 4 GiB. A surface gets a slot if created by `backboardd`, 1344x2992, 1 plane, 4 bytes/element, `'BGRA'`, `bytesPerRow == 5376`, offset 0, size <= 16 MiB. Slot is zeroed and `dc civac`'d (clean+invalidate) at assignment; memory is `withPhysicalAddress(slotPA)`.
- **Coherency**: pool surfaces read by the copy path are `dc civac`'d first only when the surface's write seed did not change since its previous presentation (the GPU wrote it); CPU-written frames are already in the cache. `gpuWritten()` implements the rule for both paths.
- **IRQs**: `husky_irq_register` (sleh table) + `husky_gic_spi_enable` (level, prio 0xA0, route to the boot CPU's MPIDR affinity, wait RWP) for INTIDs 273/274/275/288, requires `GICD_IGROUPR` bit = 1. On boot 0127d2 all four are Group 0 (Secure; Non-secure writes to IGROUPR are ignored), so `irqOn` is never called: the GIC is only read (`husky_gic_spi_probe`), flips are detected by polling `INT_PEND`/`RDMA_IRQ` every 250 us and vsync comes from `HW_TE_CNT`. The handler (`irq`) takes `fDispLock` (spin), reads `INT_PEND`, `INT_PEND_EXTRA` and `RDMA_IRQ` and runs the handler of every source that is pending whatever the INTID (as `servicePoll` does), so a re-delivered or spurious SPI finds its bit already cleared and changes no state (counted as `spurirq`, no event signal). After a handler ran it reads `INT_PEND` and `RDMA_IRQ` back so the posted W1C acks have landed and the level lines are low before EOI, then `dsb` and signals the event source. No logging, allocation, or Mach messaging in the ISR.
- **Vsync** (`pickVsync`, N >= 4): `HW_TE_CNT`, polled by `pollFired` at a quarter period, else the AppleCLCD timer. The counter is two 16-bit words (CNT_A [15:0], CNT_B [31:16]); the probe takes the first whose 500 ms delta is 20..70 (60 or 120 Hz), and every delta (`teSample`/`teWait`/`teSkip`/`pollFired`) is taken modulo 2^16 on that field. Neither in range means `hwcnt` is unusable (`use=none`) and the timer is used. `TE_RISE` is not used: the zuma driver never enables it and it never pended on silicon; `huskydisp_terise=1` retests it. `fTeTime` (last observed edge) feeds the `te-lost` watchdog. `dispTimer` starts/stops the software timer on the work loop; the shared count advances by the number of TE edges since the last send.
- **Panel 120 Hz** (`panelTo120`, N >= 5): measure TE 250 ms; if 115-125 Hz skip; otherwise require the probed `UNDERRUN_CTRL` to be `0x2235` and both halves of `CMD_TE_CTRL1` to be nearer to the 60 fps timing than to the 120 fps one (ABL's value is `0x34C53C1E`, not the formula's); quiesce (shadow request clear, 10 ms, trigger masked: `TRIG_CON` MASK 1, EN 0); send the 19-packet batch through `DSIM_PKTHDR/PAYLOAD` (polled, PKT_GO cleared if set); write the 120 fps timers (`0x06A7`, `hd_te1(120)`) and require the readback to equal them (read through `dsimTimersRead`, which reads the programming bank when `SHADOW_EN` is set and `READ_EN` is clear; the pre-write guard still reads in ABL's mode, as the probe did); wait 2 TE (`te while masked N edges` shows whether `HW_TE_CNT` counts with `HW_TRIG_EN` clear); resume in `decon_reg_start` order (frame request, clear `FRAME_START`, wait `RUN_STATUS`, unmask); measure TE 250 ms and accept 28-32 edges, else `panelTo60` (15-packet batch, the probed 60 Hz timers restored) and log FAILED.
- **Fallback** (`fallback`): clear `INT_EN` master and `EXTRA` (the frame enables and the `INT_EN_EXTRA` error enables stay, as `decon_reg_set_interrupts(0)` leaves them), mask RDMA IRQ sources, retire ARMED/SCANNING, rewrite the base to the splash and request a frame, clear the console gate, wake all waiters, re-pick vsync. Triggers: latch timeout (3 TE periods + 2 ms, one retry first; reported as `rdma-shadow` when mismatching shadow bases were seen meanwhile), SCANNING > 20 ms, 8 consecutive latch mismatches (`rdma-shadow` when the shadow value never moved, else `latch-mismatch`), DECON TIME_OUT/RESOURCE_CONFLICT, RDMA CONFIG_ERR/DEADLOCK/READ_SLAVE_ERR/FBC_ERR, `HW_TE_CNT` unchanged for 100 ms, SwapEnd armed-wait > 50 ms, retire ring overflow.

## 4. Deviations from DESIGN.md (all deliberate)

1. **Sleep/wake events live on an engine-owned `IOLock` (`fWaitLock`)**, not `AppleCLCD::fLock`, so the engine has no dependency on AppleCLCD's lock. Order is `fWaitLock` then `fDispLock`; AppleCLCD's `fLock` is only taken inside its own `sendVsync`.
2. **Bring-up runs on a dedicated kernel thread**, not the work loop: TE measurement and the 120 Hz switch sleep for seconds and must not stall the work loop that drives the vsync timer and the event source.
3. **New `Bringup` mode.** From the start of step 3 the console gate is on, so console kicks cannot race the DSIM TX or the trigger mask; swaps in that window copy into the splash and the thread requests the splash frame when it goes Direct (that first flip, `sid 0`, appears in the flip log).
4. **Step 3 also writes the DECON enables** (`INT_EN` master/EXTRA/FRAME_START/FRAME_DONE, `INT_EN_EXTRA` TIME_OUT/RESOURCE_CONFLICT, RDMA_IRQ enable with FRAME_DONE masked), the set `decon_reg_set_interrupts` writes minus the DQE dimming bits, because whether pending bits latch without them is unproven. No GIC enable, so still no CPU interrupt before step 4.
5. **DSIM timer guard is applied twice**: before the DCS batch (nothing sent, nothing to revert, log `dsim-timers`) and again after it (revert), instead of only after.
6. **Pool is allocated whenever the geometry preconditions pass** (`fPoolOK`), even if trigger/SysMMU/GIC checks keep direct mode off, so step 2 still reports CA's surfaces. `splash-in-dram` blocks both.
7. **Probe verdict line** `probe verdict pool=%d direct=%d (reason)` added; the geometry check compares RDMA `SRC_WIDTH`/`SRC_HEIGHT`/`IMG_SIZE` with the console geometry and requires `SRC_OFFSET == 0` and `fRowBytes == 4 * width`. The direct-only checks (after the pool ones) are SysMMU VM 0 off, `GLOBAL_CON` command mode, `TRIG_CON` HW trigger from DDI0 unmasked, and one enabled DECON window covering the panel on channel 0.
8. **One stats line for all steps**: `te fs fd flips copies mism reuse drops under err` plus `orphfs spurfd missfd armwait ringovf spurirq`. `drops` = TE edges coalesced into one vsync send; `under` = DSIM `INTSRC` UNDER_RUN count (N >= 5, polled by the 20 ms watchdog, W1C of bit 19 only). `spurirq` = ISR entries that found no pending source.
9. **`copies`** counts SwapEnd copies in Direct and Fallback. In Copy mode (N 1-2, or probe failed) `copyEnd` stays a no-op, so those boots show exactly today's behavior; in Fallback it adds `husky_fb_kick` as DESIGN section 10 specifies.
10. **TE period** is the mean between the first and last edge timestamps over 500 ms (ISR time of each edge), so the 16600-16720 us check is meaningful.
11. **Retire ring overflow** (work loop stalled for 8 frames) leaks that retain, sets `ringovf`, and falls back; the surface cannot be released in the ISR.
12. `husky_dcache_flush` was already non-static; only prototypes were added.
13. **cal_9865 rework, divergences kept on purpose.**
    - *RDMA format is not programmed.* The CAL pairs `DRM_FORMAT_ARGB8888` (bytes B,G,R,A, the order of CoreAnimation's `'BGRA'`) with `IDMA_IMG_FORMAT_ARGB8888` (3), while ABL scans the splash with code 0 and the CPU copy of the same `'BGRA'` bytes into the splash is what shows the UI today. Direct mode keeps ABL's code, so it shows exactly what copy mode shows; the probe accepts any linear 32-bit RGB code (0..7) and logs it. If colours turn out swapped in both modes, the fix is one `IN_CTRL_0` write for both (fix-dpp.md R10, G8).
    - *`SHD_REG_UP_REQ_CMP` is not requested* (the console's `0x80003FFF` is the proven value); `huskydisp_cmp=1` adds it.
    - *No `TE_RISE`.* Not enabled by the zuma driver, never pended on silicon; `HW_TE_CNT` is the only TE source and `te-lost` is its stall.
    - *DQE dimming interrupts (`INT_EN` [21:20]) are not enabled*; no DQE is programmed.
    - *DSIM timers*: the 120 Hz gate recognises ABL's 60 Hz timers by proximity to the CAL formula (ABL's `CMD_TE_CTRL1` does not follow it), the 120 Hz `CMD_TE_CTRL1` is the formula, the revert restores what was probed, and the write is verified by readback of the programming bank (`dsimTimersRead` sets `SHADOW_REG_READ_EN` around the read when `SHADOW_EN` is set and it is clear, because the operating bank cannot change before a frame runs; the `teWait` 28-32 edge check after resume is what proves the operating bank took the value).
    - *The 120 Hz DCS batch is unchanged and unverified*: it is the NS -> HS transition of `hk3_set_panel_feat` without the TE2 block the driver sends for it; the panel's op mode at hand-off is not probed (fix-dsim.md F3). Before this rework the TE1 gate stopped step 5 on this silicon, so the batch has never been sent here.

## 5. Test plan (one boot per row unless noted; combine with `huskyreboot=N`)

Preconditions: D-1 build K; D-2 the usual fastboot-boot flow. Logs: pmsg, grep `HUSKY DISP:`.

| Step | Boot-args | Expect | Stop and capture if |
|---|---|---|---|
| 1 | `huskydisp=1` | Boot 0127d2 values, in this order: `probe decon gcon=00000133 trig=00003061 shd=80003fff en=00003000 enx=00000011 ...`, `probe decon rsc0= rsc2= clkcon=`, `probe win5 con=00000001 start=00000000 end=0baf053f` (only window 5), `probe rdma0 en=40000004 irq= ctrl0=ff400000 w=1344 h=2992 off=00000000 img=0bb00540 base=fac00000 sbase=fac00000 st0=00000000 st1=00000000`, `probe dsim cfg=04887cff esc=01400000 urun=00002235 te0=00000050 te1=34c53c1e cmdcfg=00000000 fifo=00001507` (bit 15 may read 1), `probe dsim link3= sfr= cprs= slice01= slice23=`, `probe sysmmu ctrl=00000024`, four `probe gic 273/274/275/288 grp=0`, `probe dram ... in_dram=0`, `probe verdict pool=1 direct=1 (ok)`, `probe latch shd/pend` (8 pairs), `probe te hwcnt=%08x->%08x dA=29..31 dB=29..31 use=A`. UI unchanged. `img` is the one value the 0127d2 log never read; the verdict needs it to be `0bb00540`. | `verdict ... (geometry)`: read `w h off img`; `(base)`: `base`/`sbase` vs `splash=`; `(win)`: the `windows N enabled` line; `(rdma-fmt)`: `ctrl0`; `(trig)`/`(sysmmu)`/`(gcon)`: the raw value. `in_dram=1`: fix the shim memory map first. `use=none` or dA outside 20..70: `HW_TE_CNT` is not a TE count, vsync falls to the timer. Fix nothing blind. |
| 1+2 | `huskydisp=2` | Above plus `pool 4 slots pa=... end <= 100000000`, `slot N -> sid ... by backboardd keys=[...] name=...` per CA framebuffer, `swapwait in=3 [n 8000000X 0 0]` x32 (X in 0,1,3), `swap stats n=300 pool=.. nonpool=.. distinct=..`. UI as before (copy from pool). | `pool alloc failed`; `nonpool` dominating (see `nopool` reasons, `creator` means the process name differs); `distinct` < 3. |
| 3 | `huskydisp=3` | `decon irq en=00003011 enx=00000011 rdma_irq=...`, `direct on splash=fac00000 mask=80003fff`, `flip #1 sid 0 ...`, then `flip #n ... latch <= 17000 us xfer 5500-6500 us`, 10 s `stats` with `copies` flat after the first second, `mism=0 err=0 reuse=0`. Flips and retirement are polled (250 us), no GIC access. UI animates, no tearing. Colours are what copy mode shows. | `fallback (rdma-shadow)` (preceded by `latch mismatch armed X latched Y`): the RDMA shadow does not follow the DECON request, capture C6. `fallback (latch-timeout)` with no mismatch lines: no frame started; read the `probe latch shd/pend` samples of the same boot (pend bit 12 never set with `shd` stuck: try `huskydisp_cmp=1`, then C5). `fallback (decon-err N)` / `(rdma-err N)`: N is the pending bits. |
| 4 | `huskydisp=4` | `no IRQ mode (gic-group): SPIs are Secure, GIC untouched, flips and vsync polled` (no `irq .. on` lines on this boot path), `vsync src=hwcnt period 16600-16720 us`, stats `te ~600/10s`, `fs == fd == flips`, `orphfs=0 missfd=0 drops` small, no `husky: unhandled IRQ`. | `src=timer`: `use=none`. `fallback (te-lost)`: `HW_TE_CNT` stalled 100 ms. If a boot ever shows `irq 273/274/275/288 on`: hang on enable means an SPI storm, capture the `gic` probe rows. |
| 5 | `huskydisp=5` | `panel te 60 Hz before switch`, `dcs 19 pkts drained in <50000 us`, `te while masked N edges`, `dsim urun 2235->06a7 te1 34c53c1e->1a701e9d sfr=`, `panel 120Hz ok period 8300-8370 us`, `vsync src=hwcnt period 8300-8370`, flips `latch <= 8400 us xfer 5800-6100 us`, stats `te ~1200/10s`, `under=0`. First time the 19-packet batch runs on silicon, without the TE2 block (open item O3): watch the panel. | `panel 120Hz FAILED (...)`: it reverted to 60 Hz; read the reason. `skipped (dsim-timers urun .. te1 ..)`: the probed values are not the 60 Hz pair. `FAILED (dsim-timers readback)`: the write did not land even in the programming bank (compare `sfr=` on the `dsim urun` line with `probe dsim sfr=`; `READ_EN` is already handled). `te while masked 0 edges`: `HW_TE_CNT` stops with `HW_TRIG_EN` clear (harmless, the 2-TE wait degrades to its 100 ms cap). `under != 0`: raise MIF/INT through the DVFS boot-args and repeat. |
| 6a | `huskydisp=6 huskydisp_inject=1` | At flip 100 `latch-timeout retry pa ...`, then normal flips, no fallback. | |
| 6b | `huskydisp=6 huskydisp_inject=2` | At flip 200 `fallback (rdma-err inject) -> copy mode`, `stats copies` rising, UI still updates, vsync still TE. | |

Squeeze (memory: squeeze-boot-cycles): steps 1 and 2 can share one boot (use `huskydisp=2`). Steps 3 and 4 stay separate boots. Steps 5 and 6a can share only if 5 is already known good. The step 1 boot can also carry `huskydisp_phy=1 huskydisp_terise=1` (read-only PHY PLL read, expect en 1 p 1 m 111 s 2 k 21504 lock 1; `probe te_rise pend=N`), which answers fix-decon Q3 and fix-dsim F1 in the same boot.
Panic visibility: on the next real panic at N >= 3 the splash (panic text) should be visible; not testable on demand.

## 6. Still open (not decided by this implementation)

- **O1** (offline): SwapWait scalar order in the iOS 26 DSC (`_kern_SwapWait`, `_kern_SwapWaitWithTimeout`). The code uses in[0] = id, in[1] = timeout ms; step 2 logs the raw scalars to confirm.
- **O2** (offline): the property or call QuartzCore uses for refresh rate. None is published; at 120 Hz CA paces from the vsync callbacks alone.
- **Captures on Android** (screen on, never clk debugfs): C3 `0x19440034/84/88` at 120 and 60 Hz (confirms the DSIM timer pair); C4 `hk3_set_panel_feat` dynamic debug at 60/120 (confirms the batch); C5 `0x19470030` and `0x19470064` at 120 Hz; C6 `0x19900040` and `0x19900440` during an animation, only if step 3 shows persistent latch mismatches.
- **O3** (panel): the 120 Hz batch is NS -> HS minus the TE2 block, and the op mode at hand-off is unknown (fix-dsim.md F3, L9). The TE2 block is `B0 00 42 F2`, `F2 0D`, `B0 00 01 B9`, `B9 opt`, `B0 00 idx B9`, `B9 edges`, with the option from `hk3_get_te2_option` (a function of the driver's `feat` and `auto_mode_vrefresh`, not readable here); every mode table entry uses edges 0x10/0x30 (the NS op mode overrides the falling edge to 0x25). Not added without a capture of `hk3_set_panel_feat` at 60/120 (C4).
- **O4** (format): RDMA format 0 vs 3 for `'BGRA'` (section 4, item 13). A pure red surface settles it.
- **Probe-time unknowns the first boot will settle**: which half of `HW_TE_CNT` is the TE count (both advanced 30 per 500 ms on 0127d2; `use=` in the probe line); whether `TE_RISE` pends with the zuma enable sequence (`huskydisp_terise=1`, not needed by the engine); the DSIM PHY PLL (`huskydisp_phy=1`); the RDMA `IMG_SIZE` (0x1C) and `irq` values; the pending shadow request seen in the probe (`shd=80003fff` with no FRAME_START latched): the `probe latch` samples show whether it clears, and `huskydisp_cmp=1` tests the missing `CMP` bit; window 5's owner (`rsc0`/`rsc2`) and `clkcon`; the meaning of `RDMA_SRC_STRIDE_0/1` (logged raw, `st0`/`st1`, zero for linear); whether the RDMA shadow follows the DECON request; `TRIG_CON` bits [13:12] and [6:5] (left as ABL set them).
- **Known limits**: IOGPU submission must stay synchronous (precondition P1); frames the GPU renders into a buffer the CPU partly rewrote later can mix stale cache lines (only the seed rule guards it); pool surfaces are never freed from the pool, only recycled by slot.
