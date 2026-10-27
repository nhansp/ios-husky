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

## 1. Files changed

| File | Change |
|---|---|
| `iokit/Kernel/arm/HuskyDisplay.h` (new) | Engine class, `HuskyDisplayOps` callbacks into AppleCLCD, `husky_flip`, `HUSKY_DISP_LOG` (printf + kprintf, the husky `HU_LOG` pair). |
| `iokit/Kernel/arm/HuskyDisplay.cpp` (new) | Register maps, probe, TE measurement, scanout pool, flip engine, ISR + work-loop event source, watchdog/poll timers, DSIM TX, 60 to 120 Hz switch, fallback, stats. |
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

## 3. How it works (code map)

- **Modes** (`fMode`): `Copy` (today), `Bringup` (engine thread owns DECON0: console gate on, swaps copy into the splash silently), `Direct`, `Fallback` (sticky).
- **Flip** (`programFlip`): `RDMA BASEADDR_Y8 = pa; dsb sy; GLOBAL_CON |= 3; SHD_REG_UP_REQ = 0x80003FFF`. Nothing else is written per frame. `arm()` fills the ARMED slot; `onFrameStart` reads the RDMA shadow base (+0x440) and promotes ARMED to SCANNING only if it equals the armed PA; `onFrameDone` moves SCANNING to the retire ring (8). `doWork` (work loop) logs, releases surfaces, wakes SwapEnd/SwapWait waiters, sends vsync, applies errors.
- **SwapEnd** (`AppleCLCD::swap`): exactly one enabled layer, pool surface, src and dst rect whole-panel (a zero w/h means whole surface, as the copy loop treats it) -> `swapDirect` (seed check + `dc cvac` if CPU-written, then `armWait`, which blocks at most 50 ms while ARMED is occupied, else fallback `armed-timeout`). Otherwise `copyBegin` (wait for both slots empty), the unchanged copy loop, `copyEnd` (arm a flip of the splash; in Fallback `husky_fb_kick`; in Bringup just retire the swap id).
- **SwapWait**: blocks until `fRetiredSwapID >= in[0]`, deadline `min(in[1], 100 ms)` (100 ms without `in[1]`), `BadArgument` if the id was never begun, `Timeout` on expiry. First 32 calls log their raw scalars. Only blocks in Direct mode.
- **Pool** (`allocPool`): one contiguous `inTaskWithPhysicalMask(0xFFFFC000)` of `slots x 16 MiB`, else one per slot; every slot checked to be below 4 GiB. A surface gets a slot if created by `backboardd`, 1344x2992, 1 plane, 4 bytes/element, `'BGRA'`, `bytesPerRow == 5376`, offset 0, size <= 16 MiB. Slot is zeroed and `dc civac`'d (clean+invalidate) at assignment; memory is `withPhysicalAddress(slotPA)`.
- **Coherency**: pool surfaces read by the copy path are `dc civac`'d first only when the surface's write seed did not change since its previous presentation (the GPU wrote it); CPU-written frames are already in the cache. `gpuWritten()` implements the rule for both paths.
- **IRQs**: `husky_irq_register` (sleh table) + `husky_gic_spi_enable` (level, prio 0xA0, route to the boot CPU's MPIDR affinity, wait RWP) for INTIDs 273/274/275/288, requires `GICD_IGROUPR` bit = 1. The handler (`irq`) takes `fDispLock` (spin), reads `INT_PEND`, `INT_PEND_EXTRA` and `RDMA_IRQ` and runs the handler of every source that is pending whatever the INTID (as `servicePoll` does), so a re-delivered or spurious SPI finds its bit already cleared and changes no state (counted as `spurirq`, no event signal). After a handler ran it reads `INT_PEND` and `RDMA_IRQ` back so the posted W1C acks have landed and the level lines are low before EOI, then `dsb` and signals the event source. No logging, allocation, or Mach messaging in the ISR.
- **Vsync** (`pickVsync`, N >= 4): TE via TE_RISE IRQ if it latches and the GIC works; else the `HW_TE_CNT` field that counts TE, polled at period/4; else the AppleCLCD timer. `HW_TE_CNT` is two 16-bit counters (CNT_A [15:0], CNT_B [31:16]); the probe picks the one whose 500 ms delta matches the TE_RISE edge count (+-2), or lies in 20..70 when TE_RISE is dead, and every delta (`teSample`/`teWait`/`teSkip`/`pollFired`) is taken modulo 2^16 on that field. Neither matching means `hwcnt` is unusable (`use=none`). `dispTimer` starts/stops that timer on the work loop; the shared count advances by the number of TE edges since the last send.
- **Panel 120 Hz** (`panelTo120`, N >= 5): measure TE 250 ms; if 115-125 Hz skip; otherwise require the probed DSIM timers to be the 60 Hz pair; quiesce (shadow request clear, 10 ms, trigger masked); send the 19-packet batch through `DSIM_PKTHDR/PAYLOAD` (polled, PKT_GO cleared if set); re-check and write the 120 fps DSIM timers; wait 2 TE; unmask; request a frame; measure TE 250 ms and accept 28-32 edges, else `panelTo60` (15-packet batch, 60 Hz timers) and log FAILED.
- **Fallback** (`fallback`): mask DECON/RDMA IRQ sources (keep TE if its IRQ works), retire ARMED/SCANNING, rewrite the base to the splash and request a frame, clear the console gate, wake all waiters, re-pick vsync. Triggers: latch timeout (3 TE periods + 2 ms, one retry first; reported as `rdma-shadow` when mismatching shadow bases were seen meanwhile), SCANNING > 20 ms, 8 consecutive latch mismatches (`rdma-shadow` when the shadow value never moved, else `latch-mismatch`), DECON TIME_OUT/RESOURCE_CONFLICT, RDMA CONFIG_ERR/DEADLOCK, TE lost 100 ms, SwapEnd armed-wait > 50 ms, retire ring overflow.

## 4. Deviations from DESIGN.md (all deliberate)

1. **Sleep/wake events live on an engine-owned `IOLock` (`fWaitLock`)**, not `AppleCLCD::fLock`, so the engine has no dependency on AppleCLCD's lock. Order is `fWaitLock` then `fDispLock`; AppleCLCD's `fLock` is only taken inside its own `sendVsync`.
2. **Bring-up runs on a dedicated kernel thread**, not the work loop: TE measurement and the 120 Hz switch sleep for seconds and must not stall the work loop that drives the vsync timer and the event source.
3. **New `Bringup` mode.** From the start of step 3 the console gate is on, so console kicks cannot race the DSIM TX or the trigger mask; swaps in that window copy into the splash and the thread requests the splash frame when it goes Direct (that first flip, `sid 0`, appears in the flip log).
4. **Step 3 also writes the DECON enables** (`INT_EN` master/EXTRA/FRAME_START/FRAME_DONE, `INT_EN_EXTRA` TIME_OUT/RESOURCE_CONFLICT/TE_RISE, RDMA_IRQ enable with FRAME_DONE masked), because whether pending bits latch without them is unproven. No GIC enable, so still no CPU interrupt before step 4.
5. **DSIM timer guard is applied twice**: before the DCS batch (nothing sent, nothing to revert, log `dsim-timers`) and again after it (revert), instead of only after.
6. **Pool is allocated whenever the geometry preconditions pass** (`fPoolOK`), even if trigger/SysMMU/GIC checks keep direct mode off, so step 2 still reports CA's surfaces. `splash-in-dram` blocks both.
7. **Probe verdict line** `probe verdict pool=%d direct=%d (reason)` added; geometry check also compares RDMA `src`/`img` with the console geometry and requires `fRowBytes == 4 * width`.
8. **One stats line for all steps**: `te fs fd flips copies mism reuse drops under err` plus `orphfs spurfd missfd armwait ringovf spurirq`. `drops` = TE edges coalesced into one vsync send; `under` = DSIM `INTSRC` UNDER_RUN count (N >= 5, polled by the 20 ms watchdog, W1C of bit 19 only). `spurirq` = ISR entries that found no pending source.
9. **`copies`** counts SwapEnd copies in Direct and Fallback. In Copy mode (N 1-2, or probe failed) `copyEnd` stays a no-op, so those boots show exactly today's behavior; in Fallback it adds `husky_fb_kick` as DESIGN section 10 specifies.
10. **TE period** is the mean between the first and last edge timestamps over 500 ms (ISR time of each edge), so the 16600-16720 us check is meaningful.
11. **Retire ring overflow** (work loop stalled for 8 frames) leaks that retain, sets `ringovf`, and falls back; the surface cannot be released in the ISR.
12. `husky_dcache_flush` was already non-static; only prototypes were added.

## 5. Test plan (one boot per row unless noted; combine with `huskyreboot=N`)

Preconditions: D-1 build K; D-2 the usual fastboot-boot flow. Logs: pmsg, grep `HUSKY DISP:`.

| Step | Boot-args | Expect | Stop and capture if |
|---|---|---|---|
| 1 | `huskydisp=1` | Probe lines for decon/win/rdma0/dsim/sysmmu/gic x4/dram, `probe verdict pool=1 direct=1 (ok)`, `probe te pend=29..31 or 59..61`, `hwcnt=%08x->%08x dA=.. dB=.. use=A|B|none` (the used field's delta equals `pend` +-2). UI unchanged. | `verdict ... (base\|geometry\|trig ...)`: read the line, fix nothing blind. `in_dram=1`: fix the shim memory map first. `pend=0` and `enx_forced=1` still 0: TE_RISE dead, expect HWCNT or timer later. |
| 1+2 | `huskydisp=2` | Above plus `pool 4 slots pa=... end <= 100000000`, `slot N -> sid ... by backboardd keys=[...] name=...` per CA framebuffer, `swapwait in=...` x32, `swap stats n=300 pool=.. nonpool=.. distinct=..`. UI as before (copy from pool). | `pool alloc failed`; `nonpool` dominating (see `nopool` reasons, `creator` means the process name differs); `distinct` < 3. |
| 3 | `huskydisp=3` | `direct on splash=fac00000 mask=80003fff`, `flip #1 sid 0 ...`, then `flip #n ... latch <= 17000 us xfer 5500-6500 us`, 10 s `stats` with `copies` flat after the first second, `mism=0 err=0 reuse=0`. UI animates, no tearing. | `fallback (rdma-shadow)` (preceded by `latch mismatch armed X latched Y`): the RDMA shadow does not follow the DECON request, capture C6. `fallback (latch-timeout)` with no mismatch lines: no frame started (check TE/trigger, C5). |
| 4 | `huskydisp=4` | `irq 273/274/275/288 on (prio a0 route ...)`, `vsync src=te period 16600-16720 us`, stats `te ~600/10s`, `fs == fd == flips`, `orphfs=0 missfd=0`, no `husky: unhandled IRQ`. `spurirq` is informational (non-zero only means a level SPI was taken after its source was already acked). | No `irq` lines: `no IRQ mode (gic-group)` (a probe row shows `grp=0`). `src=hwcnt` or `src=timer`: TE_RISE dead; flips still polled-correct. Hang on enable: SPI storm, capture `gic` probe rows. |
| 5 | `huskydisp=5` | `panel te 60 Hz before switch`, `dcs 19 pkts drained in <50000 us`, `dsim urun 2235->06a7 te1 34e13d3b->1a701e9d`, `panel 120Hz ok period 8300-8370 us`, `vsync src=te period 8300-8370`, flips `latch <= 8400 us xfer 5800-6100 us`, stats `te ~1200/10s`, `under=0`. | `panel 120Hz FAILED (...)`: it reverted to 60 Hz; read the reason. `skipped (dsim-timers urun .. te1 ..)`: ABL's values differ from the table, capture C3 and replace the pair. `under != 0`: raise MIF/INT through the DVFS boot-args and repeat. |
| 6a | `huskydisp=6 huskydisp_inject=1` | At flip 100 `latch-timeout retry pa ...`, then normal flips, no fallback. | |
| 6b | `huskydisp=6 huskydisp_inject=2` | At flip 200 `fallback (rdma-err inject) -> copy mode`, `stats copies` rising, UI still updates, vsync still TE. | |

Squeeze (memory: squeeze-boot-cycles): steps 1 and 2 can share one boot (use `huskydisp=2`). Steps 3 and 4 stay separate boots. Steps 5 and 6a can share only if 5 is already known good.
Panic visibility: on the next real panic at N >= 3 the splash (panic text) should be visible; not testable on demand.

## 6. Still open (not decided by this implementation)

- **O1** (offline): SwapWait scalar order in the iOS 26 DSC (`_kern_SwapWait`, `_kern_SwapWaitWithTimeout`). The code uses in[0] = id, in[1] = timeout ms; step 2 logs the raw scalars to confirm.
- **O2** (offline): the property or call QuartzCore uses for refresh rate. None is published; at 120 Hz CA paces from the vsync callbacks alone.
- **Captures on Android** (screen on, never clk debugfs): C3 `0x19440034/84/88` at 120 and 60 Hz (confirms the DSIM timer pair); C4 `hk3_set_panel_feat` dynamic debug at 60/120 (confirms the batch); C5 `0x19470030` and `0x19470064` at 120 Hz; C6 `0x19900040` and `0x19900440` during an animation, only if step 3 shows persistent latch mismatches.
- **Probe-time unknowns the first boot will settle**: whether `INT_PEND_EXTRA.TE_RISE` and `HW_TE_CNT` count with `te_from = DDI0`, and which half of `HW_TE_CNT` (CNT_A or CNT_B) it is (`use=` in the probe line; the zuma CAL defines both halves and never reads them); the meaning of `RDMA_SRC_STRIDE_0/1` (logged raw, `st0`/`st1`); whether the RDMA shadow follows the DECON request.
- **Known limits**: IOGPU submission must stay synchronous (precondition P1); frames the GPU renders into a buffer the CPU partly rewrote later can mix stale cache lines (only the seed rule guards it); pool surfaces are never freed from the pool, only recycled by slot.
