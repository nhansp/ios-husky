#!/usr/bin/env python3
"""Pack an xnu kernel Mach-O + ADT into the payload the husky shim boots.

  mkpayload.py KERNEL ADT OUT [--cmdline "..."] [--ramdisk IMAGE]

The kernel is an arm64e PIE with LC_DYLD_CHAINED_FIXUPS in the
DYLD_CHAINED_PTR_ARM64E_KERNEL (format 7) chain format. Nobody else (no
kmutil, no iBoot) resolves those for us, so the chains are walked here using
XNU's own algorithm (osfmk/mach/dyld_kernel_fixups.h: rebase_chain):

  - plain rebases are written as their final VA (no slide: the kernel runs
    at its link VAs, the shim just maps phys_base -> virt_base);
  - auth (PAC) rebases have 42k+ entries that must be signed with the
    kernel's PAC keys. That cannot be done offline without QARMA5, so each
    auth slot is written with its *unsigned* target and recorded in an
    auth-fixup side table; the shim signs them in place at runtime with the
    CPU (loader.c), using the fixed architected keys patch 0002 installs.

Payload layout (little endian, offsets from payload start):
  0x000  char  magic[8] = "HUSKYXNU"
  0x008  u32   version = 2, nsegs
  0x010  u64   virt_base      kernel VA that maps to phys_base
  0x018  u64   entry_va       LC_UNIXTHREAD pc
  0x020  u64   adt_off, adt_size
  0x030  u64   kernel_end_va  end of the highest segment (page aligned)
  0x038  u64   phys_base      kernel-managed RAM (ADT dram-base/size)
  0x040  u64   mem_size
  0x048  u64   auth_off       offset of the auth-fixup table
  0x050  u64   auth_count     number of auth-fixup records
  0x058  char  cmdline[1024]
  0x458  seg[nsegs] { u64 vmaddr, vmsize, file_off(in payload), file_size }
  auth_off: auth_count * { u64 slot_va, u32 info, u32 pad }   (16 bytes each)
            info = diversity[0:15] | addrDiv<<16 | key<<17   (key 0=IA 1=IB 2=DA 3=DB)

--ramdisk appends IMAGE as one more segment after the kernel and moves
kernel_end past it, so the image lands below the ADT and boot_args, inside
topOfKernelData, and the ADT gets /chosen/memory-map "RAMDisk" = <phys,size>.
"""
import argparse, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

LC_SEGMENT_64, LC_UNIXTHREAD, LC_DYLD_CHAINED_FIXUPS = 0x19, 0x5, 0x80000034
PAGE = 0x4000
BOOT_LINE_LENGTH = 1024
VERSION = 2
MAGIC = b'HUSKYXNU'
AUTH_OFF_FIELD = 0x48
CMDLINE = 0x58
SEG_TABLE = CMDLINE + BOOT_LINE_LENGTH	# 0x458

# DYLD_CHAINED_PTR_ARM64E_KERNEL (format 7): 4-byte stride, arm64e rebase/auth
# bit layout shared with osfmk/mach/dyld_kernel_fixups.h rebase_chain().
ARM64E_KERNEL = 7


def parse_macho(buf):
    magic, cputype, cpusub, ftype, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<8I', buf, 0)
    assert magic == 0xfeedfacf, 'not a 64-bit Mach-O'
    segs, entry, fixups = [], None, None
    off = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', buf, off)
        if cmd == LC_SEGMENT_64:
            name = buf[off + 8:off + 24].split(b'\0')[0].decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<4Q', buf, off + 24)
            segs.append(dict(name=name, vmaddr=vmaddr, vmsize=vmsize, fileoff=fileoff, filesize=filesize))
        elif cmd == LC_UNIXTHREAD:
            # arm_thread_state64: x0..x28, fp, lr, sp, pc, cpsr -> pc at +16+32*8
            entry, = struct.unpack_from('<Q', buf, off + 16 + 32 * 8)
        elif cmd == LC_DYLD_CHAINED_FIXUPS:
            fixups = struct.unpack_from('<II', buf, off + 8)	# dataoff, datasize
        off += size
    return dict(cputype=cputype, cpusub=cpusub & 0xff, segs=segs, entry=entry, fixups=fixups)


def sext(v, bits):
    """Sign-extend the low `bits` of v to 64 bits."""
    m = 1 << (bits - 1)
    return ((v & ((1 << bits) - 1)) ^ m) - m


def apply_chained_fixups(buf, m):
    """Resolve every chained rebase in buf. Returns the auth-fixup table
    [(slot_va, key, addrDiv, diversity)] for runtime signing; plain rebases
    are written to their final VA in place. Follows rebase_chain() exactly:
    auth target = low32 + image_base; non-auth target = (high8<<56) |
    sign_extend43(low43); no slide (the kernel runs at its link VAs)."""
    auth = []
    if not m['fixups']:
        return auth
    data_off, _ = m['fixups']
    _, starts_off, _, _, imports_count, _, _ = struct.unpack_from('<7I', buf, data_off)
    assert imports_count == 0, 'kernel has binds; only rebases are supported'
    image_base = m['segs'][0]['vmaddr']		# __TEXT, holds the mach header
    seg_count, = struct.unpack_from('<I', buf, data_off + starts_off)
    for i in range(seg_count):
        seg_info, = struct.unpack_from('<I', buf, data_off + starts_off + 4 + 4 * i)
        if not seg_info:
            continue
        base = data_off + starts_off + seg_info
        size, page_size, fmt, seg_offset, max_valid, page_count = struct.unpack_from('<IHHQIH', buf, base)
        assert fmt == ARM64E_KERNEL, f'unsupported pointer format {fmt} (expected {ARM64E_KERNEL})'
        starts = struct.unpack_from('<%dH' % page_count, buf, base + 22)
        seg = m['segs'][i]
        for p, start in enumerate(starts):
            if start == 0xffff:		# DYLD_CHAINED_PTR_START_NONE
                continue
            assert not start & 0x8000, 'DYLD_CHAINED_PTR_START_MULTI not supported'
            loc = seg['fileoff'] + p * page_size + start
            while True:
                raw, = struct.unpack_from('<Q', buf, loc)
                is_auth = raw >> 63
                nxt = (raw >> 51) & 0x7ff
                slot_va = seg['vmaddr'] + (loc - seg['fileoff'])
                if is_auth:
                    target = ((raw & 0xffffffff) + image_base) & 0xffffffffffffffff
                    key = (raw >> 49) & 3
                    addr_div = (raw >> 48) & 1
                    diversity = (raw >> 32) & 0xffff
                    auth.append((slot_va, key, addr_div, diversity))
                else:
                    top8 = (raw >> 43) & 0xff
                    target = ((top8 << 56) | (sext(raw & 0x7ffffffffff, 43) & 0x00ffffffffffffff)) & 0xffffffffffffffff
                struct.pack_into('<Q', buf, loc, target)
                if not nxt:
                    break
                loc += nxt * 4		# ARM64E_KERNEL stride
    return auth


def add_ramdisk_to_adt(adt_blob, phys, size):
    root, _ = adt.parse(adt_blob)
    chosen = next(c for c in root.children if c.name == 'chosen')
    mmap = next(c for c in chosen.children if c.name == 'memory-map')
    assert mmap.prop('RAMDisk') is None, 'ADT already has a RAMDisk'
    mmap.props.append(('RAMDisk', 0, struct.pack('<2Q', phys, size)))
    return adt.serialize(root)


def adt_dram(adt_blob):
    root, _ = adt.parse(adt_blob)
    chosen = next(c for c in root.children if c.name == 'chosen')
    return (struct.unpack('<Q', chosen.prop('dram-base'))[0],
            struct.unpack('<Q', chosen.prop('dram-size'))[0])


def build(kernel_path, adt_path, cmdline, ramdisk_path=None):
    buf = bytearray(open(kernel_path, 'rb').read())
    m = parse_macho(buf)
    auth = apply_chained_fixups(buf, m)
    segs = [s for s in m['segs'] if s['vmsize']]
    text = m['segs'][0]
    assert text['name'] == '__TEXT' and text['fileoff'] == 0
    # virt_base: 32MB (16K-granule L2 block) below the mach header, so the
    # block mapping that begins at the header's block begins at phys_base.
    virt_base = text['vmaddr'] & ~(0x2000000 - 1)
    kernel_end = max(s['vmaddr'] + s['vmsize'] for s in segs)
    kernel_end = (kernel_end + PAGE - 1) & ~(PAGE - 1)

    adt_blob = open(adt_path, 'rb').read()
    PHYS_BASE, MEM_SIZE = adt_dram(adt_blob)	# the ADT decides the kernel's RAM
    pieces = [(s['vmaddr'], s['vmsize'], buf[s['fileoff']:s['fileoff'] + s['filesize']]) for s in segs]
    ramdisk = None
    if ramdisk_path:
        image = open(ramdisk_path, 'rb').read()
        rd_size = (len(image) + PAGE - 1) & ~(PAGE - 1)
        rd_va = kernel_end
        kernel_end = rd_va + rd_size
        rd_pa = rd_va - virt_base + PHYS_BASE
        pieces.append((rd_va, rd_size, image))
        adt_blob = add_ramdisk_to_adt(adt_blob, rd_pa, rd_size)
        ramdisk = (hex(rd_pa), hex(rd_size))

    hdr_size = SEG_TABLE + 32 * len(pieces)
    out = bytearray(hdr_size)
    seg_table = []
    for vmaddr, vmsize, data in pieces:
        off = len(out)
        out += data
        out += b'\0' * (-len(out) & 0xf)
        seg_table.append((vmaddr, vmsize, off, len(data)))
    # auth-fixup table, 16-aligned; each record padded to 16 bytes so the u64
    # slot_va stays 8-aligned (the shim is built -mstrict-align) and the stride
    # matches the loader's natural struct auth_fixup layout.
    auth_off = len(out)
    for slot_va, key, addr_div, diversity in auth:
        out += struct.pack('<QII', slot_va, diversity | (addr_div << 16) | (key << 17), 0)
    out += b'\0' * (-len(out) & 0xf)
    adt_off = len(out)
    out += adt_blob
    out += b'\0' * (-len(out) & 0xf)

    struct.pack_into('<8sII', out, 0, MAGIC, VERSION, len(pieces))
    struct.pack_into('<7Q', out, 0x10, virt_base, m['entry'], adt_off, len(adt_blob), kernel_end,
                     PHYS_BASE, MEM_SIZE)
    struct.pack_into('<2Q', out, AUTH_OFF_FIELD, auth_off, len(auth))
    cl = cmdline.encode()[:BOOT_LINE_LENGTH - 1]
    out[CMDLINE:CMDLINE + len(cl)] = cl
    for i, t in enumerate(seg_table):
        struct.pack_into('<4Q', out, SEG_TABLE + 32 * i, *t)
    info = dict(cputype=hex(m['cputype']), cpusub=m['cpusub'], virt_base=hex(virt_base),
                entry=hex(m['entry']), kernel_end=hex(kernel_end), phys_base=hex(PHYS_BASE),
                mem_size=hex(MEM_SIZE), auth_fixups=len(auth), auth_off=hex(auth_off),
                ramdisk=ramdisk, size=len(out),
                segs=[(s['name'], hex(s['vmaddr']), hex(s['vmsize'])) for s in segs])
    return bytes(out), info


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('kernel'); ap.add_argument('adt'); ap.add_argument('out')
    ap.add_argument('--cmdline', default='debug=0xc serial=3 cpus=1 keepsyms=1')
    ap.add_argument('--ramdisk', help='root image for md0 (boot with rd=md0)')
    a = ap.parse_args()
    blob, info = build(a.kernel, a.adt, a.cmdline, a.ramdisk)
    open(a.out, 'wb').write(blob)
    for k, v in info.items():
        if k != 'segs':
            print(f'{k}: {v}')
    for s in info['segs']:
        print('  ', *s)
