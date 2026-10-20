# Porting XNU + an iOS 26 userland to Pixel 8 Pro (husky)

**Status:** design, pending approval · **Date:** 2026-10-20
**Codename convention:** `husky` = Pixel 8 Pro, SoC `zuma` = Google Tensor G3

## Intent & why

A public project, `ios-sweet`, boots an iOS 16 userland on a Qualcomm phone (Redmi
Note 10 Pro) by compiling Apple's **open-source XNU** (APSL) from source with its
in-kernel integrity enforcement compiled permissive, and running it on non-Apple
hardware via the stock unlocked bootloader. No Apple on-device security is
circumvented because none is present on non-Apple silicon — the kernel is simply
rebuilt without the enforcement it would normally carry.

This project applies that technique to a newer, harder target: **Tensor G3 hardware
and an iOS 26 userland.** It is bring-up / OS-research, not a product. The value is
the port engineering: a fresh SoC family (Exynos-derived), a newer XNU generation
(`xnu-12377`), and the pointer-authentication / PPL realities of the iOS 26 kernel.

## Goal (definition of done)

**husky boots to the iOS 26 SpringBoard home screen, unlockable, with working
touch**, driven by XNU built from `xnu-12377` source, debugged live over the
device's USB-C serial console. Equivalent to sweet's achieved milestone, one
XNU generation newer, on Tensor.

## Non-goals (explicitly out of scope — hardware-blocked, not effort-blocked)

- **GPU acceleration** — husky has a Mali GPU; iOS's Metal/AGX stack has no Mali
  driver. Rendering stays CPU-side (CoreAnimation software), as on sweet.
- **Cellular / RIL** — husky's modem is a Samsung Exynos `g5300i`; iOS's baseband
  stack speaks Apple's proprietary protocol to Apple-qualified basebands. No modem
  to talk to.
- **Secure Element / Apple Pay / Face ID / SEP** — require Apple's SEP, physically
  absent. AppleKeyStore is stubbed (as sweet does).
- **Wi-Fi / Bluetooth** — both husky and iOS are Broadcom, so a bridge is
  *conceivable*, but it is a large separate effort and not part of this goal.
- **Daily-driver use.**

## Anti-goals (would make this a failure even if it "worked")

- Any dependency on Apple online services (activation uses the offline
  "Hactivation" path, as sweet does).
- **Touching the bootloader/EL3 firmware stages — `abl`, `bl1/bl2/bl31`, `pbl`,
  `tzsw`, `ldfw`, `gsa`, `pvmfw`, modem, persist. HARD line: modifying any of these
  can hard-brick the phone, so we never write them — a probe that would require it
  (e.g. EL3 gating PAC off) is a dead end, not a detour.** The bootloader is
  unlocked; `boot`, `vbmeta`, `dtbo` and the data partition are fair game
  (recoverable by re-flashing the factory image), so flashing `boot` / patching
  `vbmeta` is an allowed fallback if `fastboot boot` turns out AVB-gated.

## Hard constraint: the userland is an input, not a build artifact of this project

The iOS 26 root filesystem is Apple's proprietary software. Producing it from a
user-supplied `iPhone12,5` iOS 26.x IPSW is done **outside this repo with standard
community IPSW tooling** (`ipsw` CLI / `pyimg4`), the same position sweet takes with
its `mkiosroot.sh`. Note iOS 26 ships OS/SystemOS as `.aea` archives (vs sweet's
plain DMGs) — handled entirely by that external tooling. **This project's code
consumes a finished root image and the device `/product` DeviceTree as inputs** and
does not implement extraction, decryption, or repackaging.

## Target, decided

| Decision | Value | Why |
| --- | --- | --- |
| iOS version | **26 (`xnu-12377`)** | Newest XNU source Apple has published; 27 has no source tag yet, so it cannot be built. Confirmed IPSW: 26.6.2 / 23G90. |
| Impersonated device | **iPhone 11 Pro Max — `iPhone12,5` / `d431ap`** | Oldest-gen iOS-26 class → **A13 = PPL, not SPTM**. 1242×2688 panel fits husky's 1344×2992 with least letterbox. Face-ID gesture UI matches sweet's model. |
| Security model inherited | **PPL + arm64e/PAC** | A13 predates SPTM/TXM/exclaves (A15+). PAC is mandatory at iOS 26 (userland is arm64e-only) — the standout new cost vs sweet's non-PAC iPhone X build. |
| Base | sweet's approach & SoC-independent patches | ~half the patch series is board-agnostic. |

## Hardware map (husky / zuma — from the factory device trees)

| Block | Value | vs sweet | Plan |
| --- | --- | --- | --- |
| DRAM base | `0x8000_0000` | different | shim MMU (16K granule) |
| Interrupt ctrl | `arm,gic-v3 @ 0x1040_0000` | also GICv3 | reuse sweet's GICv3 path |
| Debug UART | `ttySAC0 @ 0x1087_0000`, exynos4210 mmio32 | sweet had none | real serial console (big win) |
| UFS | boot UFS **`ufs@1320_0000`** (`androidboot.boot_devices=13200000.ufs`); DTS also lists `ufs@1302_0000` | Qualcomm → Exynos | rewrite host driver |
| Display | `decon@1947_0000`, `dsim@1944_0000`, panel `google,hk3` | MDSS → DECON/DSIM | ABL-framebuffer blit |
| Touch | **`st,fts`** over SPI / touch-bus-negotiator | Goodix GENI → ST FTS | rewrite driver |
| PMIC | **`s2mpg14mfd` + `s2mpg15mfd`** over bit-bang SPMI | Qualcomm RPMh/SPMI → S2MPG | buttons + rail enables |
| Watchdog | `watchdog_cl0@1006_0000`, `cl1@1007_0000` | — | map for `wdt=-1` |
| Bootloader log | `google,bldr_log @ 0xfd80_0000` | — | persistent-log backstop |
| CPUs | 4×A510 (0-3) + 4×A715 (4-7) + 1×X3 (8); **A510 boots** | 6×A55 homog. | tri-cluster PSCI SMP |

## Boot chain

```
ABL (Exynos bootloader, fastboot flashing unlock'd; DECON/DSIM; supports
     `fastboot oem uart enable` + `oem uart config 3000000`)
  -> fastboot boot <shim boot.img v4>            (temporary; nothing flashed)
     -> shim (start.S + C): 16K-granule MMU, init UART @ 0x10870000, read live
        FDT (DRAM layout), load XNU + synthesized Apple Device Tree + ramdisk
        -> XNU (xnu-12377, arm64e/PAC, PPL de-fanged, d431ap ADT, boot-args)
           -> Exynos UFS driver -> HFS+ iOS root in data partition
              -> dyld + arm64e shared cache -> launchd -> daemons
                 -> backboardd -> SpringBoard
```

Debug is strictly better than sweet: synchronous kernel output over `ttySAC0`
from the shim's first instruction (needs a USB-C serial dongle), with the
`bldr_log` region as a backstop. No filming the screen.

## The two novel risks (vs sweet) — retire them first with throwaway spikes

1. **arm64e / PAC on Tensor.** The iOS 26 kernelcache and userland are arm64e;
   PAC is mandatory. Apple XNU manages PAC keys via Apple-specific EL3/GXF paths
   and may touch Apple-proprietary system registers that don't exist on Tensor's
   standard ARMv8.3 PAuth. Risk: XNU's PAC setup faults on Tensor, or pointers do
   not round-trip. **Spike:** build a minimal PAC-enabled XNU for d431, boot under
   the shim, confirm PAC sign/auth round-trips on an A715/X3 core and that no
   Apple-only SPR is required (replace with standard `APIAKeyLo/Hi_EL1` etc. if so).
2. **PPL de-fang at iOS 26.** On A13 iOS 26 uses PPL (Page Protection Layer), gated
   on Apple silicon by APRR/GXF that Tensor lacks. We must compile the PPL
   enter/exit paths to run as ordinary kernel code (no hardware gating to satisfy),
   a more invasive version of what sweet does for AMFI/trust-cache. **Spike:**
   confirm a PPL-defanged `xnu-12377` reaches the kernel console.

If either spike stalls, documented fallback: bring up the **same husky board layer
under iOS 16 (`xnu-8796`, iPhone X)** to prove the Tensor drivers independently of
the iOS-26 security model, then return to 26. (This is the "iOS 16 first" option
deferred earlier, kept as a de-risking fallback, not the primary path.)

## Patch-reuse analysis (sweet's 81-patch series)

- **Port ~as-is (SoC-independent):** toolchain/build fixes; closed-Apple header &
  export stubs; kmem RNG / HMAC-PRNG; trust-cache-unavailable; builtin pthread;
  BearSSL + portable AES/SHA; mockfs ramdisk; AMFI-permissive; builtin HFS;
  AppleKeyStore stub; sandbox policy; coredump-off; crash/console logging; IOKit
  trace. (~the "security-off + closed-gap + logging" core.)
- **Rewrite for Tensor:** framebuffer/serial console (→ real UART + DECON blit);
  machine-config & device-tree platform expert; boot-CPU driver; physmap alignment;
  UFS (the largest driver); IOSurface/IOMobileFramebuffer (re-target DECON/`hk3`);
  HID touch (ST FTS) / buttons (S2MPG) / battery; SMP/PSCI (tri-cluster); USB;
  device identity + display size (`iPhone12,5`, 1242×2688).
- **New (no sweet equivalent):** PAC enablement; PPL de-fang. *The real work.*

## Bring-up sequence (phases)

- **P0 — Spikes (throwaway):** retire the scary unknowns cheaply —
  - (a) **Delivery probe:** confirm retail unlocked husky honors `fastboot boot` of a
    standalone image *with nothing flashed* (gates the whole no-flash model — see
    Decision 1).
  - (b) **Shim + UART:** shim prints over `ttySAC0` from the first instruction +
    reads the live FDT. Confirms the serial dongle path works.
  - (c) **PAC spike — run on the A510 boot core, not just a big core** (Q7): confirm
    stock ARMv8.3 PAuth round-trips; confirm retail `bl31` leaves `SCR_EL3.API/APK`
    enabled for EL1/EL0 with keys delegated to EL1 (if EL3 gates PAC off, the only
    fix touches `bl31` — see Decision 1). Extend to the **userland-facing PAC
    contract**: signed thread state across exception return / `sigreturn`,
    per-process key diversity (Q2) — the part that actually gates P3.
  - (d) **PPL-defang + source audit:** confirm the d431/A13 build in public
    `xnu-12377` compiles the PPL (`XNU_MONITOR`) path and that PPL isn't half-gated
    behind SPTM assumptions (Q1); defang its enter/exit to plain kernel code.
  - (e) **Framebuffer-persistence probe (Q5):** does ABL leave a continuously-scanned
    framebuffer after `fastboot boot` hands off with no Android kernel servicing
    DECON? If not, P4 grows a from-scratch DECON/DSIM/`hk3` bring-up.
- **P1 — Early kernel:** shim loads stripped `xnu-12377` (d431 ADT) → kernel console
  over UART, GICv3 + timers + boot CPU up.
- **P2 — Storage + root:** Exynos UFS driver; mount HFS+ iOS root; pivot root.
- **P3 — Userland (headless):** dyld shared cache + launchd + daemons run (AMFI
  permissive, keystore stub, Hactivation). "iOS is alive" over serial.
- **P4 — Display:** IOMFB/sweetfb blit to ABL framebuffer; CoreAnimation software
  render; Apple logo → boot progress → lock screen visible.
- **P5 — Input (= the goal):** ST FTS touch + S2MPG buttons → unlock → SpringBoard.
- **P6 — Stretch:** more cores (SMP), battery readout, stability.

## Environment / inputs required

- Unlocked husky; a USB-C serial dongle; a Linux PC.
- GitHub macOS runners for the XNU build (`blacktop/darwin-xnu-build`, extended to
  `xnu-12377` / arm64e).
- A finished iOS 26.x (`iPhone12,5`) root image + the `d431ap` `/product`
  DeviceTree, produced externally with standard IPSW tooling (see constraint above).
- The `gs-6.1` google-modules tree as the Tensor driver reference (UFS, DECON,
  ST FTS touch, S2MPG PMIC, SMP).

## Builder-check resolutions (2026-10-20)

Settled during review and folded above:
- **PAC/PPL exist in public `xnu-12377`** (`pac_asm.h`, `HAS_APPLE_PAC`,
  `ml_set_kernelkey_enabled`) → de-fang, not reconstruct. Caveat: the 26 kernel is
  SPTM-centric; confirming the d431 PPL build path is now an explicit P0(d) audit.
- **Matching HID kext source is published** (`IOHIDFamily-2238.120.5` / `2222.x`,
  the iOS 26 line). ABI drift from sweet's `1915.140.3` is P4/P5 porting effort.
- **Spike scope corrected:** PAC spike runs on the **A510 boot core** and covers the
  **userland PAC contract** + the **EL3/`bl31` PAuth-enablement** check, not just
  kernel-internal round-trip on a big core.
- **Reference-tree gap:** the supplied `gs-6.1 google-modules` snapshot looks
  Pixel-9-era — it has zuma SoC bits (`cal-if/zuma`, UFS/MFD under `soc/gs`) but the
  **Pixel 8 Pro `hk3` panel driver is absent** (`display/panels/` has only
  caimito/stallion/tegu). SoC drivers are usable; the display fallback needs the
  husky-matched branch — see Decision 2.

## Decisions (resolved 2026-10-20)

1. **Brick line is HARD on the bootloader/EL3 stages, soft on recoverable
   partitions** (see anti-goals). Consequence baked into P0: the `bl31`/EL3-PAC
   check in P0(c) is pass/fail-to-abort — if EL3 traps PAC for EL1 we stop, we do
   not patch `bl31`. (Mitigating fact: Pixel `bl31` is ARM TF-A-derived, which
   normally leaves `SCR_EL3.API/APK` set so PAC isn't trapped at lower ELs — so this
   is a tail risk, not an expected one.) If `fastboot boot` is AVB-gated, flashing
   `boot` / patching `vbmeta` is the allowed detour.
2. **Panel source:** the supplied `lineage-24.0` tree has the zuma SoC display
   driver (`cal_9865`) but not the `hk3` panel driver; `hk3` to be sourced from
   `android-gs-shusky-6.1` only if P0(e) forces a from-scratch panel bring-up.
   Manifest for reference:
   `LineageOS/android_kernel_google_gs-6.1_manifest@lineage-24.0`.
3. **Hardware:** husky is connected (adb/fastboot over USB, `adb reboot bootloader`
   available). USB-C SBU **serial dongle not yet explicitly confirmed** — required
   for P0(b); P0(a) needs only fastboot. Confirm dongle before P0(b).

## iOS 26 root recon (2026-10-20, from the extracted `iOSRoot` HFS+ image)

Confirmed against the assembled root (`artifacts/ios26-root-23G90.img`, HFSX,
478,602 entries, `ProductVersion 26.6.2 / 23G90 / "iPhone OS"`):

- **dyld shared cache is `arm64e`-only** (`dyld_shared_cache_arm64e` + `.01`–`.04`
  subcaches, plus a DriverKit cache). No arm64 slice → **PAC is mandatory**, hardening
  risk #1 from a maybe to a fact.
- **Cache lives in the cryptex path** `private/preboot/Cryptexes/OS/System/Library/
  Caches/com.apple.dyld/`, not the classic `/System/Library/Caches/com.apple.dyld`.
  On-device these are signature-verified cryptex disk images mounted by the
  preboot/cryptex machinery. We merged them in as plain dirs, so a **new P3 task**:
  make dyld resolve its cache without a real cryptex mount (firmlinks/binds/symlinks)
  — a meaner version of sweet's "add dyld cache links."
- **628 LaunchDaemons.** Bring-up disable-list (P3 root-tweaks, the sweet
  `sweet-root-tweaks.py` analog, scaled up): `CommCenter*` / `WirelessRadioManager` /
  `DumpBasebandCrash` (no modem), `wifid` / `bluetooth*` (no radio), `locationd`
  (no GPS), `watchdogd` (kills slow procs during soft-render boot),
  `analyticsd`/`aggregated`/`cloudd`/`apsd`/`identityservicesd`/`itunescloudd`
  (network/crash loops), `appleidsetupd`/`mediasetupd` (skip Setup),
  `mobileactivationd` → offline Hactivation.
- **Goal binaries present:** `SpringBoard.app` + `SpringBoard.framework` + backboardd;
  MobileGestalt daemon present (consumes the `d431ap` `/product` device tree).

## Bootloader (LK/ripcurrent) dmesg recon (2026-10-20, via `fastboot oem dmesg`)

`fastboot oem dmesg` dumps the full bootloader log over fastboot — a second debug
channel independent of the UART. Key facts:

- **Display is DSI command-mode + DSC — changes P4.** Panel `google-hk3`, `cmd mode`,
  DECON `command mode, hw trigger`, path `DSCC DSC_ENC1 DSC_ENC0`. ABL configures a
  full-res window: `xres(1344) yres(2992)`, `win_end_pos 0xbaf053f` = (2991<<16)|1343.
  Unlike sweet's video-mode continuously-scanned FB, husky's panel self-refreshes its
  last frame from GRAM and **each update needs a DECON frame trigger (TE) + DSC
  encode**. So the display path is not "write pixels into a scanned buffer" — it's
  "write raw frame to DECON source + trigger a (DSC-encoded) frame." Bigger P4 task;
  the FB *source* address is still TBD (P0e), but geometry + pipeline are known.
- **UFS:** Samsung UFS 3.1, controller **v3.0**, caps `0x1383ff1f`, `KLUDG4UHGC-B0E1`,
  128 GB, 4 LUNs, boot LUN `0x2`, WriteBooster present — host-driver reference.
- **PMIC:** `S2MPG_M` (S2MPG14 main) + `S2MPG_S` (S2MPG15 sub), chipid 0x3; PWRONSRC/
  OFFSRC/INT registers logged — buttons/rail reference.
- **Boot chain order confirmed:** BL1 → PBL → **BL2** → EL3(`bl31`) → ABL(LK). (`bl2`
  is a real stage — correctly in the never-touch set.)
- Misc: SoC `0x09865` Zuma rev B1; USB `dwc3`; TCPC/PD `max77759`; haptics `cs40l26`;
  NFC `st21nfc`; modem `g5300i` (Exynos — non-goal). Slot `b` active.
- **Debug channels:** `fastboot oem dmesg` covers the bootloader stage. The *live* UART
  (`ttySAC0 @ 0x10870000`, P0b) needs a host-visible serial endpoint — currently
  `lsusb` shows only the fastboot device, no USB-serial bridge / `/dev/ttyUSB*`, so the
  dongle path must be sorted before the shim-UART spike.

## Live Android recon (rooted, 2026-10-20) — `artifacts/live-recon/`

Booted stock Android (`fastboot continue`) and harvested `dmesg`/`iomem`/`interrupts`
with root. Decisive findings:

- **PAC confirmed + working: `Address authentication (architected QARMA5 algorithm)`.**
  Risk #1 recast: the arm64e userland's PAC is runtime-computed, so QARMA5 round-trips
  it; the job is **retarget Apple's PAC key-management (Apple SPRs/GXF) to architected
  `APIAKey_EL1`-class regs**, not make PAC exist. Much lower risk than feared.
- **CPU topology (live):** CPU0–3 `0x411fd461` (A510, **boot CPU = little A510**),
  CPU4–7 `0x411fd4d0` (A715), CPU8 X3. Heterogeneous `ID_AA64MMFR1_EL1` across clusters
  → XNU SMP must tolerate non-uniform ID regs (it assumes Apple-homogeneous clusters).
- **GICv3:** GICD `0x10400000`; redistributor **stride `0x20000`** from `0x10440000`.
- **PSCI v1.1 / SMC conduit / SMCCC v1.2** → PSCI SMC CPU bring-up (sweet patch 0065).
- **Timers:** arch timer (`arch_sys_counter`) + exynos-mct per-CPU IRQs 790–798.
- **RAM 12 GB**; **UFS** live at `0x13200000` (+ PHY/UNIPRO windows `…13280000/…132a0000`).
- **ABL enters the kernel at EL2** (`CPU: All CPU(s) started at EL2`; husky is GKI).
  pKVM (`kvm-arm.mode=protected`, `pkvm_s2mpu-v9`) is something *Linux* installs into
  EL2 after entry, so it is absent when we `fastboot boot` our own image: the shim owns
  EL2 and drops to EL1 itself (sweet's Qualcomm ABL entered at EL1 — not the case here).
  **DPU S2MPU** at `0x19870000/0x19c70000` gates display DMA; its state as left by ABL
  is still to be probed (P4).

### Debug-strategy correction (supersedes the earlier "serial = jackpot")
The AP UART (`ttySAC0 @ 0x10870000`) exits via USB-C **SBU pins** and needs a debug
adapter (SuzyQ-type) on the host; with only a charging cable there is **no live serial**.
So the realistic debug model:
- **Bootloader stage:** `fastboot oem dmesg`.
- **Android stage:** adb + root `dmesg`/`logcat` (used for this recon).
- **Bare-metal shim/XNU stage (no adb, no serial):** use the device's **ramoops
  persistent RAM at `0x400000 @ 0xfd3ff000`** (confirmed live: pstore backend,
  `/sys/fs/pstore/console-ramoops-0` readable). Shim/XNU writes its console there in
  ramoops record format → reboot to Android → `cat /sys/fs/pstore/console-ramoops-*`.
  Serial-free, works on the charging cable; slower turnaround (reboot per read). The
  framebuffer console is a poor fallback here (command-mode + DSC panel). A ~$15
  SuzyQ-type USB-C debug cable would restore live UART and speed bring-up — optional.
- **Fastest serial-free loop (bootloader-side, no Android boot) — confirmed in ABL RE:**
  ABL parses pstore/ramoops (`[PSTORE]`/`[RAMOOPS]`/`pstore_pram_update`) and exposes
  `fastboot oem ramdump stage_file <region>` + `fastboot get_staged <file>` to pull a
  memory region to the host. So: shim/XNU writes ramoops → reboot bootloader →
  `oem ramdump stage_file <region>` + `get_staged`. (`fastboot oem dmesg` may also merge
  a `[RAMOOPS]` section when present.) Exact `<region>` token (dmesg/console/ramoops/
  pstore/debug_snapshot log name) TBD by a one-time live probe against the existing
  `console-ramoops-0`. Bonus ABL verbs: `oem dbgc uart-mux`/`uart-console` (debug-UART
  routing for a future SuzyQ), plus a USB-ramdump panic dumper.

## Shim P0a (2026-10-20) — `shim/`

Ground truth from the stock images and LineageOS `device/google/zuma`
`BoardConfig-common.mk` (lineage-24.0):
- `BOARD_BOOT_HEADER_VERSION := 4`; stock `boot.img` is **kernel-only** (ramdisk in
  `init_boot`); **DTB lives in `vendor_kernel_boot`** (stock `vendor_boot` dtb size 0).
  So the shim ships as a kernel-only v4 `boot.img` and `fastboot boot` still gets the
  device's own zuma FDT in `x0`.
- Kernel cmdline has `earlycon=exynos4210,0x10870000 console=ttySAC0,115200`.
- **16 KB-page developer option** (`TARGET_KERNEL_DIR/16kb`, separate `dtbo`): Google
  ships a 16K-granule EL1 kernel on this SoC/ABL, which largely retires "does 16K
  translation work on all three core types" for XNU.
- ramoops (live `/sys/module/ramoops/parameters`): `mem 0x400000@0xfd3ff000,
  record_size 0, console_size 2M, pmsg_size 2M, ecc 0` → per `fs/pstore/ram.c` the
  **console zone is `0xfd3ff000`..+2M**, a `persistent_ram_buffer` `{sig 0x43474244,
  start, size, data}`; Linux keeps it if `start <= size <= 2M-12`.

P0a shim (`shim/`, ported from `ios-sweet/shim/`): EL2→EL1 drop modelled on Linux
`init_kernel_el` for an nVHE host (HCR_EL2 RW|API|APK, timers/GIC sysregs/PMU
untrapped, FGT/HCRX cleared when present), text log in the ramoops console zone,
reports EL, load address, FDT header, ID registers, PSCI/SMCCC, then warm-resets
(SYSTEM_RESET2 if offered). Pass = the log shows up in `console-ramoops-0`.

## P0a/P0b results (2026-10-20) — PASS on hardware

Delivery gate (the real one): husky ABL chains `boot` verification to the
boot image's own vbmeta footer, so an unsigned kernel-only `boot.img` is
**rejected before the first instruction runs** (ABL resets to fastboot in
~3 s). This looked identical to "shim crashed," and cost most of the
session to isolate. The fix is to reflash the root `vbmeta` with
verification off:

```
fastboot --disable-verity --disable-verification flash vbmeta vbmeta.img
fastboot -w            # verity state change invalidates FBE userdata
```

The disable flags are only honoured in the **root** `vbmeta`; it chains to
`boot`, `init_boot`, `vbmeta_system` (different key), `vbmeta_vendor`, and
disabling at the root cuts all chains, so `vbmeta_system` needs no change.
`vbmeta.img` is the build-matched one for CP41.260831.019 (rollback index
1791158400), pulled from the OTA with `scratchpad/ota_pull_partition.py`.
After this, the shim is a plain unsigned v4 `boot.img` — no footer, no
padding — delivered by `fastboot boot` (nothing flashed).

Debug channel that works: write the shim log to the ramoops **pmsg** zone
`0xfd5ff000` (not the console zone `0xfd3ff000`, which ABL overwrites with
its own log on the very next boot). As the first act at `_start`, before
the EL drop can fault, the shim asks EL3 to set the Pixel reboot mode to
NORMAL; `fastboot boot` leaves the mode at BOOTLOADER, so if the shim runs
at all the next PSCI `SYSTEM_RESET` boots Android instead of fastboot, and
`adb shell cat /sys/fs/pstore/pmsg-ramoops-0` reads the log back. Serial-
free, survives the handoff, no root needed (shell is in the `log` group).

Measured (`artifacts/p0a/shim-pmsg-log.txt`):

| What | Value | Note |
| --- | --- | --- |
| Entry / current EL | `entry=2 now=1` | ABL enters at EL2; **EL2→EL1 drop works** |
| EL3 priv-reg SMC | `BOOTMODE 0` | reboot-mode write succeeds |
| Load address | `0x8000_0000..0x8001_4000` | DRAM base, 81 KB image |
| DTB in x0 | `0xb400_0000`, FDT magic `d00dfeed`, size `0x73dc8` | **ABL passes a valid ~475 KB FDT** |
| `/chosen` initrd | `0xb000_0000..0xb311_72a7` | ramdisk range parsed by `fdt.c` |
| Boot CPU MIDR | `0x411fd461` | **A510 (little) boots first** |
| PAC (`ISAR1`) | APA=5, GPA=1 | **QARMA5 address + generic auth** |
| Granule/PA (`MMFR0`) | TGran16=1, PARange=2 | **16 KB granule**, 40-bit PA |
| PSCI / SMCCC | `0x0001_0001` / `0x0001_0002` | **v1.1 / v1.2** |

Net: delivery, EL handoff, FDT read, PSCI/SMCCC, and the de-risking CPU
features (16K granule, PAC/QARMA5) are all confirmed on-device. Next:
P0c PAC spike proper (sign/authenticate round-trip at EL1, EL3
PAuth-contract check) and the PPL source audit of xnu-12377, then P1
early-kernel bring-up (loader.c load of the XNU Mach-O + synthesized ADT).

## P0c PAC spike (2026-10-20) — PASS on hardware

On the A510 boot core, at EL1 after the EL2->EL1 drop (pac.S + shim.c,
`artifacts/p0a/shim-pac-log.txt`):

| Check | Result |
| --- | --- |
| Enable pac/aut | `SCTLR_EL1 = 0xf8d02980` (En{IA,IB,DA,DB} set) |
| EL3 PAuth contract | `pacia 0x12345670 -> 0xa229738b12345670` — a real QARMA5 signature, so `pac`/`aut` are **not trapped to EL3** (`SCR_EL3.API=1`) |
| Architected keys | `APIAKeyLo/Hi_EL1`, `APGAKeyLo/Hi_EL1` settable at EL1 and used |
| Determinism | two signings identical |
| Round-trip | `autia` recovers `0x12345670` exactly |
| Generic MAC | `pacga -> 0x57739fb4_00000000` |
| FPAC | corrupt `autia` faults: SYNC, `ESR 0x72000000` (EC=0x1C, FEAT_FPAC), caught by our EL1 vectors |

Consequence for XNU: the arm64e PAC path is architecturally available and
correct here. Apple's kernel programs the keys through Apple-private SPRs
and GXF/exception-gating that don't exist on zuma; the port retargets
those to the architected `AP*Key_EL1` regs proven above (sweet had no PAC
analog, so this is new code, but bounded — key plumbing, not crypto). FPAC
being on means a mis-signed pointer traps rather than silently corrupting,
matching iOS behaviour, so the kernel's PAC exception handling must be
wired before arm64e userland runs.

## XNU-12377 source audit + build strategy (2026-10-20)

Three source audits of `artifacts/xnu-12377` (tag 12377.121.6). Headline: the
kernel is built in one of two mutually-exclusive configs (`osfmk/conf/files.arm64`),
and we build the classic one, synthesizing a board modeled on **VMAPPLE** — the
architected/virtual config — not any real Apple SoC.

### Monitor: build `config_pmap_ppl`, with the monitor OFF
- `config_sptm` is **unbuildable from open source**: it links SDK-only
  `-lsptm_xnu` / `-lTrustedExecutionMonitor_Kernel` and `<sptm/sptm_xnu.h>`
  (absent from the tree), and its `_start` expects the SPTM already running with
  MMU on — an undocumented, un-spoofable ABI.
- `config_pmap_ppl` is fully present (`osfmk/arm/pmap/*`, `arm/arm_init.c`,
  `arm64/arm_vm_init.c`, `arm64/start.s`). Its `_start` takes **x0 = boot_args
  physical, EL1, MMU/caches off, and sets up its own MMU** — exactly the state
  our `el2_to_el1` drop produces. The shim jumps straight in.
- **No-monitor pmap is first-class**: with `XNU_MONITOR`/`PMAP_CS` undefined,
  every public pmap routine calls `*_internal()` in-kernel (186 `#if XNU_MONITOR`
  guards in pmap.c, all with `#else` fallbacks; `pmap_internal.h:112`). VMAPPLE
  does exactly this.
- **CTRR/AMCC lockdown compiles out** when `HAS_CTRR/HAS_KTRR` are undefined
  (`amcc_rorgn.h:39`, `machine_routines.c:609`); no Apple-memory-controller MMIO
  is touched. VMAPPLE does this too.

### Board: synthesize a VMAPPLE-class board (not A13/H12)
- A13 (`H12`/`t8030`) was **removed** from this tree (oldest is `H13`=A14). A13
  *cores* survive in `cpuid.h` (`CPU_PART_LIGHTNING/THUNDER`), but there is no
  board config. We don't need one: husky is not an Apple SoC.
- `VMAPPLE.h` already declares husky's reality: `NO_MONITOR`, `HAS_GIC_V3`,
  `__ARM_16K_PG__`, no `HAS_CTRR/KTRR`, `CPU_HAS_APPLE_PAC`. Base the husky board
  on it: add `ARM64_BOARD_CONFIG_*` in `board_config.h` + `MakeInc.def`, drop the
  `hvc`-to-host paravirt bits (`HAS_PARAVIRTUALIZED_PAC/CTRR`), and plug in the
  architected-register PAC backend (below). XNU's **native GICv3 driver** (VMAPPLE)
  serves husky's GIC directly — sweet's GIC patch is unnecessary.
- The **userland identity stays independent**: ADT `/product` + `compatible` =
  `d431ap`/`iPhone12,5` (already extracted to `artifacts/adt/d431ap.adt`). The
  A13/iPhone-11 userland runs unchanged on a VMAPPLE-class kernel; the SoC posed
  is a userland/MobileGestalt choice, not a kernel one.

### PAC retarget (fills the gap P0c proved)
Apple's private PAC SPRs are **gone** from open XNU (moved into SPTM); nothing to
rewrite. The key-programming asm macros are deliberately **empty** off the
paravirt path. The port fills them with the architected `msr AP*Key*_EL1` writes
validated in P0c:
- boot key init: `pac_asm.h:148` `PAC_INIT_KEY_STATE`
- per-process keys: `pac_asm.h:99/129` `SET_{JOP,ROP}_KEY_REGISTERS`; enable the
  cswitch gates `cswitch.s:227-228`
- kernel-signs-user: `machine_routines_asm.s:107-137` `ml_{enable,disable}_user_jop_key`
- diversifiers: `machine_routines_apple.c:233-269` return real values (now 0)
- use the non-SPTM pmap's in-tree signing (`arm/pmap/pmap.c:9584`
  `pmap_sign_user_ptr_internal`), never the SPTM `sptm_sign_user_pointer`.
`pacia`/`pacga` sites and `SCTLR_EL1.En{IA,IB,DA,DB}` are already architected.

### Handoff contract (updates loader.c / mkpayload.py)
- `_start` (`arm64/start.s`): x0 = boot_args **physical**, EL1, MMU off. No CRC or
  version gate on arm64 boot_args — a hand-built struct is accepted (set Rev=2/Ver=2).
- **boot_args grew**: `BOOT_LINE_LENGTH=1024`, struct **1152 bytes** (`boot.h`,
  `genassym.c:366`). sweet's `loader.c`/`mkpayload.py` assumed 608/736 → update.
  Field offsets through `CommandLine` (0x6c) unchanged; `bootFlags`/`memSizeActual`
  at 0x470/0x478.
- ADT **panic-if-missing** set: `/chosen` with `dram-base`+`dram-size` (u64) and
  `random-seed`; a `chosen/memory-map` node; `/cpus/cpuN` each with `state`
  (one `"running"`) + `reg`; under `arm-io` an `interrupt-controller=="master"`
  + `device_type=="timer"` with `reg`. (`mkadt.py` covers most; reconcile sweet's
  `gic` node with stock `master`/`timer` matching, or use VMAPPLE's GICv3 ADT shape.)

### Forward-looking (iOS 27/28, other SoCs)
PAC is present on every A12+ device, so it is never the gate. The gates are whether
a given XNU source drop still ships `config_pmap_ppl` and `VMAPPLE`. VMAPPLE is load-
bearing for macOS VMs on Apple Silicon, so it is likely to persist; `config_pmap_ppl`
is the one Apple may eventually drop once all shipping devices are SPTM. The SoC a
future port poses as (A14, A15, …) remains a userland-fit choice, decoupled from this.

## P1.1 result (2026-10-20) — HUSKY kernel builds

`kernel.development.husky` builds end to end on CI (nhansp/ios-husky,
`.github/workflows/xnu.yml`): Mach-O 64-bit **arm64e** (caps PAK00), PIE,
21.7 MB. `xnu/patches/0001-husky-machine-config.patch` adds the HUSKY board
(MakeInc.def, board_config.h, HUSKY.h, pexpert Makefile manifest, proc_reg.h,
start.s SCTLR skip, and doconf marking soc "husky" SOC_IS_VIRTUALIZED).

Shake-out that got here (each one mechanical, surfaced by CI): ccache
wrapper must be a single-executable script (xcodebuild execs CC as one path)
— now opt-in/off pending a deployment-target interaction; HUSKY.h must be in
pexpert's EXPORT_MD_LIST manifest; SOC_IS_VIRTUALIZED (via doconf) disables
the Apple-CPMU/CTRR class husky lacks; the KDK's closed KernelSupport archive
(aliased libHUSKY -> libVMAPPLE in the workflow) is coupled to VMAPPLE's exact
config, so **HUSKY is built at VMAPPLE parity** (HUSKY.h = `#define HUSKY 1` +
`#include VMAPPLE.h`; SME/paravirt kept, runtime-gated on the ID regs). A tiny
`xnu-errors` artifact carries a deduped error report so the full ~400k-line
log never has to be pulled.

Next (P1.2): diverge from parity as targeted patches on this green build —
the architected-PAC key programming (fill the empty `pac_asm.h` macros with
the `msr AP*Key_EL1` writes validated in P0c; drop VMAPPLE's paravirt hvc
PAC path) and the real MMIO/topology via the synthesized ADT — then the
loader/mkpayload/mkadt port to the 12377 boot_args/ADT contract.

## Left to the builder

- Exact ABL framebuffer address/format (one live `fastboot boot` probe).
- Which `xnu-12377.x` point tag to pin against the chosen 26.x userland.
- ~~`boot.img` v4 packaging specifics~~ — resolved, see *Shim P0a* below.
- ST FTS SPI bring-up specifics and S2MPG register map for buttons/rails
  (from the `gs-6.1` drivers).
