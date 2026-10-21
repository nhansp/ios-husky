#!/usr/bin/env python3
"""Build the minimal Apple Device Tree the husky xnu needs to boot.

Only what the early boot path actually reads; every MMIO address comes from
the husky hardware map (Tensor G3 "zuma"). Usage:
  mkadt.py OUT.adt [--dump] [--highmem] [--boot-mpidr HEX] [--timebase HZ]

The kernel board is VMAPPLE-class (GICv3 + 16K pages + architected PAC); the
userland identity it poses as is the iPhone 11 Pro Max (iPhone12,5 / d431ap),
whose /product node is spliced in from the extracted Apple device tree
(artifacts/adt/d431ap.adt).

Two values differ per unit and are best read from the hardware: the boot
core's MPIDR affinity and the timer frequency. The shim logs mpidr_el1 and
cntfrq_el0 on every run; pass the observed values via --boot-mpidr/--timebase
to pin cpu0's reg and the timebase exactly. The defaults below are the
expected Tensor G3 values.
"""
import hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

# Kernel-managed RAM. husky's DDR is banked: a low bank at 0x80000000 and high
# banks at 0x880000000+ (0x100000000 is an unbacked GAP - jumping xnu there
# fault-fetches). The low bank holds firewall-protected coprocessor carve-outs
# (from /proc/device-tree/reserved-memory): gsa@90200000 (GSA - EL1-NS access
# faults), gxp_*@91xxxxxx, tpu_fw@93000000, aoc@94000000, i.e. a protected
# block ~0x90200000-0x95000000, plus the log/ramoops/abl region at 0xf8800000+.
# So the kernel window sits BELOW the coprocessor block: 0x82000000 (above the
# shim+payload image, which ends ~0x81600000) up to just under gsa, 32MB-block
# aligned.
PHYS_BASE = 0x82000000
MEM_SIZE = 0x0d000000	# -> ends 0x8f000000, ~2MB clear of gsa@90200000
# --highmem: the 4GB-mark bank, an unbacked gap (faults); kept for experiments.
LOW_PHYS_BASE = 0x100000000
LOW_MEM_SIZE = 0x80000000

# /arm-io children are relative to this; it must not be 0 (serial_init bails).
SOC_BASE = 0x01000000

# GICv3: GICD 0x10400000, GICR 0x10440000 (stride 0x20000 per redistributor).
# 9 cores -> 9*0x20000 = 0x120000; round the window up to cover them.
GICD, GICD_SIZE = 0x10400000, 0x10000
GICR, GICR_SIZE = 0x10440000, 0x140000

# Timer: architected generic timer. CNTFRQ is read back by the shim; 24.576MHz
# is the expected Tensor value. Virtual timer PPI11 -> GIC INTID 27.
TIMEBASE = 24576000
VTIMER_INTID = 27

# Boot core: the little cluster (Cortex-A510, MIDR 0x411fd461) boots first.
# Its MPIDR affinity (masked, what XNU matches against the cpu reg) is read by
# the shim; 0 is the expected value for core 0 of cluster 0.
BOOT_MPIDR = 0x0

# Display: DECON command-mode panel (google,hk3), 1344x2992. No linear boot
# framebuffer is assumed here; the shim fills boot_args.Video from the device
# FDT if ABL left one, otherwise XNU comes up headless (console via the shim's
# pmsg log for bring-up).
CANVAS_W, CANVAS_H = 1344, 2992

HUSKY_ECID = 0x4855534b59000001  # 'HUSKY' + 1, synthetic
BOOT_MANIFEST_HASH = hashlib.sha384(b'husky boot manifest').digest()

# iPhone 11 Pro Max userland (iPhone12,5 / D431AP). The /product node is an
# Apple artifact (kept out of git): extracted to artifacts/adt/d431ap.adt.
D431AP_ADT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../artifacts/adt/d431ap.adt')

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def d431ap_product(path=D431AP_ADT):
    """/product properties of the real D431AP tree, without the syscfg
    placeholders iBoot would fill in (flag bit set) and the node bookkeeping."""
    if not os.path.exists(path):
        print(f'warning: {path} missing, no /product node', file=sys.stderr)
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == b'product\0':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def build(seed=None, highmem=False, boot_mpidr=BOOT_MPIDR, timebase=TIMEBASE):
    phys_base, mem_size = (LOW_PHYS_BASE, LOW_MEM_SIZE) if highmem else (PHYS_BASE, MEM_SIZE)
    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))
    serial_phandle = next(phandle)   # fixed id: the 'defaults' serial-device points here

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('firmware-version', cstr('husky-shim')),
        ('system-firmware-version', cstr('husky-shim')),
        # iBoot's SHA-384 of the boot manifest; /sbin/mount -P 1 needs it.
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        # libimg4 (cryptexd) aborts without a crypto hash method.
        ('crypto-hash-method', cstr('sha2-384')),
        # 1344x2992 @3x. Scale 0 makes SpringBoard's display mode invalid.
        ('display-scale', u32(3)),
        # IOPlatformUUID (gethostuuid) derives from these; synthetic ECID (not
        # the phone's), chip-id of the A13 the iPhone 11 userland expects.
        ('unique-chip-id', struct.pack('<Q', HUSKY_ECID)),
        ('chip-id', u32(0x8030)),
        # Dev-fused, undemoted board (0,0,1,1): mobileactivationd takes its
        # native Hactivation branch (no SEP, no baseband here).
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        # Local boot policy (kern_csr.c): SIP relaxed like `csrutil disable`;
        # lp-sip1 unauthenticated (unsealed) root for the extracted iOS cache.
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # husky SMP: 4x Cortex-A510 (little, cores 0-3), 4x Cortex-A715 (cores
    # 4-7), 1x Cortex-X3 (core 8). The kernel defaults to one core and takes
    # cpus=N as an initial prefix. cpu0's reg must match the boot MPIDR.
    CORES = [('a510', 0), ('a510', 1), ('a510', 2), ('a510', 3),
             ('a715', 0), ('a715', 1), ('a715', 2), ('a715', 3),
             ('x3', 0)]
    cpu_nodes = []
    for idx, (kind, in_cluster) in enumerate(CORES):
        cluster = {'a510': 0, 'a715': 1, 'x3': 2}[kind]
        reg = boot_mpidr if idx == 0 else (cluster << 8) | in_cluster
        cpu_nodes.append(node(f'cpu{idx}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr('arm,cortex-' + kind) + cstr('ARM,v8')),
            ('reg', u32(reg)),             # MPIDR Aff1:Aff0, NOT the logical id
            ('cpu-id', u32(idx)),          # XNU assigns ids by DT order
            ('cluster-id', u32(cluster)),
            ('cluster-type', u32(0)),      # treat as homogeneous SMP for bring-up
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(in_cluster)),
            ('l2-cache-id', u32(cluster)),
            ('l3-cache-id', u32(0)),
            ('state', cstr('running' if idx == 0 else 'stopped')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32(2914000000 if kind == 'x3' else 2368000000 if kind == 'a715' else 1803000000)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(3200000000)),
            ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)), ('#size-cells', u32(0))], cpu_nodes)

    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-v3')),
        # pe_arm_map_interrupt_controller looks the PIC up by this value; without
        # it pe_arm_init_interrupts bails before ml_init_timebase.
        ('interrupt-controller', cstr('master')),
        ('virtual-timer-intid', u32(VTIMER_INTID)),
        ('reg', u64(GICD - SOC_BASE, GICD_SIZE, GICR - SOC_BASE, GICR_SIZE)),
    ])
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('husky-io')),
        ('compatible', cstr('husky-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [gic,
        # pe_arm_map_interrupt_controller also requires a device_type "timer"
        # node and maps its reg, but arm64 uses the architected timer and never
        # touches it; point it at the GICD page so the mapping is harmless.
        node('timer', [ph(), ('device_type', cstr('timer')),
                       ('reg', u64(GICD - SOC_BASE, 0x4000))]),
        # husky,pmsg-console: pe_serial.c routes the kernel console into the
        # ramoops pmsg zone (0xfd5ff000); 'defaults' serial-device selects it.
        node('serial0', [('AAPL,phandle', u32(serial_phandle)),
                         ('device_type', cstr('serial')),
                         ('compatible', cstr('husky,pmsg-console'))]),
        # watchdogd checks /arm-io/wdt for no-pmu, else crashes.
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        # IOMobileFramebuffer counts /arm-io children with this device_type and
        # reads the canvas size; AppleCLCD matches the compatible string.
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('husky,display')),
            ('canvas-width', u32(CANVAS_W)),
            ('canvas-height', u32(CANVAS_H)),
        ])])

    # pmap_compute_max_asids needs the node; no-effaceable-storage makes
    # keybagd read systembag.kb as a plain plist (no effaceable key here).
    # serial-device points serial_init at our ramoops pmsg console (the node is
    # under arm-io below); XNU's kprintf/panic then lands in pmsg-ramoops-0.
    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1)),
                                 ('serial-device', u32(serial_phandle))])

    # MobileGestalt HomeButtonType = /buttons home-button-type (2 = no Home
    # button, matching the iPhone 11 Pro Max).
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),
        ('opposed-power-vol-buttons', u32(1)),
    ])

    product_props = d431ap_product()
    product = [node('product', [ph()] + product_props)] if product_props else []

    # Identity of the iPhone 11 Pro Max whose userland runs here: compatible[0]
    # is hw.model/hw.target (D431AP), model is hw.machine (iPhone12,5).
    # 'google,husky'/'google,zuma' stay for HuskyPlatformExpert matching.
    return node('device-tree', [
        ph(),
        ('compatible', cstr('D431AP') + cstr('iPhone12,5') + cstr('google,husky') + cstr('google,zuma') + cstr('AppleARM')),
        ('model', cstr('iPhone12,5')),
        ('target-type', cstr('D431')),
        ('target-sub-type', cstr('D431AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, buttons] + product)


if __name__ == '__main__':
    bm = BOOT_MPIDR
    tb = TIMEBASE
    args = sys.argv[1:]
    if '--boot-mpidr' in args:
        bm = int(args[args.index('--boot-mpidr') + 1], 0)
    if '--timebase' in args:
        tb = int(args[args.index('--timebase') + 1], 0)
    root = build(highmem='--highmem' in args, boot_mpidr=bm, timebase=tb)
    blob = adt.serialize(root)
    out = args[0]
    open(out, 'wb').write(blob)
    if '--dump' in args:
        adt.dump(adt.parse(blob)[0])
    print(f'{out}: {len(blob)} bytes  (boot_mpidr={hex(bm)} timebase={tb})', file=sys.stderr)
