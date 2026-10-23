#!/usr/bin/env python3
"""Symbolicate HUSKY process-sampler lines (xnu patch 0069) from a console log.

  husky-smp-sym.py LOG [DSC]

LOG holds lines printed by bsd/dev/husky_sample.c:
  SMP name[pid] slide 0x... mh 0x...
  SMP name tN pc 0x... lr 0x... sp 0x... bt 0x... 0x...
Each pc/lr/bt address is unslid with the shared-cache slide of its process
and looked up in the dyld shared cache with `ipsw dyld a2s`. Addresses that
are not in the cache are printed as an offset from the main binary's Mach-O
header (mh), for lookup in the binary from the root image.
"""
import re
import subprocess
import sys

DSC_DEFAULT = 'artifacts/research/dsc/cache/dyld_shared_cache_arm64e'
HDR = re.compile(r'SMP (\S+)\[(\d+)\] slide (0x[0-9a-f]+) mh (0x[0-9a-f]+)')
THR = re.compile(r'SMP (\S+) (t\d+) pc (0x[0-9a-f]+) lr (0x[0-9a-f]+) sp 0x[0-9a-f]+ bt(.*)')

_cache = {}


def lookup(dsc, addr, slide, mh):
    if addr in _cache:
        return _cache[addr]
    unslid = addr - slide
    out = subprocess.run(['ipsw', 'dyld', 'a2s', dsc, hex(unslid), '-i', '-d'],
                         capture_output=True, text=True).stdout
    image = next((l.split('>', 1)[1].split('(')[0].strip().rsplit('/', 1)[-1]
                  for l in out.splitlines() if l.strip().startswith('>')), None)
    sym = next((l.split(': ', 1)[1].strip() for l in reversed(out.splitlines())
                if l.startswith(hex(unslid) + ':')), '?')
    if image:
        res = f'{image}!{sym}' if sym != '?' else f'{image}!? (unslid {unslid:#x})'
    else:
        res = f'main+{addr - mh:#x}' if addr >= mh else f'? {addr:#x}'
    _cache[addr] = res
    return res


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    dsc = sys.argv[2] if len(sys.argv) > 2 else DSC_DEFAULT
    procs = {}
    for line in open(sys.argv[1], errors='replace'):
        m = HDR.search(line)
        if m:
            procs[m.group(1)] = (int(m.group(3), 16), int(m.group(4), 16))
            print(f'== {m.group(1)}[{m.group(2)}] slide {m.group(3)} mh {m.group(4)}')
            continue
        if 'SMP ---' in line:
            print(line.strip()[line.find('SMP'):])
            continue
        m = THR.search(line)
        if not m or m.group(1) not in procs:
            continue
        slide, mh = procs[m.group(1)]
        print(f'  {m.group(1)} {m.group(2)}')
        frames = [('pc', m.group(3)), ('lr', m.group(4))]
        frames += [('bt', a) for a in m.group(5).split()]
        for kind, a in frames:
            print(f'    {kind} {a}  {lookup(dsc, int(a, 16), slide, mh)}')


if __name__ == '__main__':
    main()
