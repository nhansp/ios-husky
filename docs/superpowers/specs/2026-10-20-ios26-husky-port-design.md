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
- Touching husky's bootloader/modem/persist partitions. We only ever
  `fastboot boot` a temporary image and write the iOS root to the data partition.

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
| UFS | `ufs@1302_0000`, `ufs@1320_0000` | Qualcomm → Exynos | rewrite host driver |
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

- **P0 — Spikes (throwaway):** (a) shim prints over `ttySAC0` + reads FDT; (b) PAC
  spike; (c) PPL-defang spike. Retire the scary unknowns cheaply.
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

## Left to the builder

- Exact ABL framebuffer address/format (one live `fastboot boot` probe).
- Which `xnu-12377.x` point tag to pin against the chosen 26.x userland.
- `boot.img` v4 packaging specifics for the shim on husky.
- ST FTS SPI bring-up specifics and S2MPG register map for buttons/rails
  (from the `gs-6.1` drivers).
