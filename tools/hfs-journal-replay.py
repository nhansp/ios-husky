#!/usr/bin/env python3
"""Replay the journal of an HFS+ image (left dirty by a force-reboot) and mark it empty.

  hfs-journal-replay.py IMAGE            # replay in place
  hfs-journal-replay.py --dry-run IMAGE  # only report what would be replayed

Linux's hfsplus driver ignores the journal; editing a volume with a dirty
journal lets XNU replay stale transactions over the edits at the next mount.
This replays exactly the transactions between the journal header's start and
end (format and checks follow replay_journal() in hfs-715 core/hfs_journal.c),
transaction by transaction: a transaction's blocks are written only once all of
its block lists verified, and replay stops at the first bad one. XNU's optional
"extra transactions past end" recovery is not done; any prefix of committed
transactions is a consistent volume. Then start = end in the journal header.
"""
import struct
import sys

JNL_MAGIC = 0x4a4e4c78
ENDIAN_MAGIC = 0x12345678
BLHDR_CHECK_CHECKSUMS = 0x1
BLHDR_FIRST_HEADER = 0x2
BLHDR_CHECKSUM_SIZE = 32
JHDR_CKSUM_SIZE = 44        # offsetof(journal_header, sequence_num)


def calc_checksum(data):
    c = 0
    for b in data:
        c = ((c << 8) ^ (c + b)) & 0xffffffff
    return (~c) & 0xffffffff


def main():
    dry = '--dry-run' in sys.argv
    path = [a for a in sys.argv[1:] if not a.startswith('--')][0]
    f = open(path, 'rb' if dry else 'r+b')

    f.seek(1024)
    vh = f.read(512)
    sig, _ver, attr, _lmv, jib = struct.unpack('>2sHIII', vh[:16])
    bsize = struct.unpack('>I', vh[40:44])[0]
    if sig not in (b'H+', b'HX') or not attr & 0x2000:
        sys.exit(f'not a journaled HFS+ volume (sig {sig}, attr {attr:#x})')
    f.seek(jib * bsize)
    ji = f.read(64)
    jflags, = struct.unpack('>I', ji[:4])
    joff, jsize = struct.unpack('>QQ', ji[36:52])
    if jflags & 0x2:            # kJIJournalOnOtherDeviceMask
        sys.exit('journal on another device: not supported')

    f.seek(joff)
    hdr = bytearray(f.read(512))
    magic, endian, start, end, size, blhdr_size, hcksum, jhdr_size, seq = \
        struct.unpack('<iiqqqiIiI', hdr[:48])
    if magic != JNL_MAGIC or endian != ENDIAN_MAGIC:
        sys.exit(f'unexpected journal header magic {magic:#x} endian {endian:#x}')
    tmp = bytearray(hdr[:JHDR_CKSUM_SIZE])
    tmp[36:40] = b'\0\0\0\0'
    if calc_checksum(tmp) != hcksum:
        sys.exit('journal header checksum mismatch')
    if start == size:
        start = jhdr_size
    if end == size:
        end = jhdr_size
    print(f'journal at {joff:#x}, size {size}, jhdr_size {jhdr_size}, blhdr_size {blhdr_size}, '
          f'start {start}, end {end}, seq {seq}')
    if start == end:
        print('journal is empty: nothing to replay')
        return

    def jread(off, n):
        """Read n bytes at journal offset off, wrapping past size to jhdr_size."""
        out = b''
        while n:
            if off >= size:
                off = jhdr_size + (off - size)
            take = min(n, size - off)
            f.seek(joff + off)
            out += f.read(take)
            off += take
            n -= take
        if off >= size:
            off = jhdr_size + (off - size)
        return out, off

    def wrap(off):
        return jhdr_size + (off - size) if off >= size else off

    committed, pending = [], []
    ntx = nbl = 0
    last_seq = 0
    off = start
    stop_reason = 'reached end'
    while off != end:
        raw, data_off = jread(off, blhdr_size)
        bl = bytearray(raw)
        orig, = struct.unpack('<I', bl[8:12])
        bl[8:12] = b'\0\0\0\0'
        if calc_checksum(bl[:BLHDR_CHECKSUM_SIZE]) != orig:
            stop_reason = f'bad block list header checksum at {off}'
            break
        max_blocks, num_blocks, bytes_used, _ck, flags = struct.unpack('<HHiIi', raw[:16])
        bseq = struct.unpack('<I', raw[28:32])[0]          # binfo[0].u.bi.b.sequence_num
        if last_seq and bseq and bseq not in (last_seq, last_seq + 1):
            stop_reason = f'sequence {bseq} out of order after {last_seq} at {off}'
            break
        if (max_blocks <= 0 or max_blocks > size // jhdr_size or num_blocks <= 0 or
                num_blocks > max_blocks or bytes_used <= 0):
            stop_reason = f'bad looking block list at {off} (max {max_blocks} num {num_blocks})'
            break
        if flags & BLHDR_FIRST_HEADER:
            committed += pending        # previous transaction is complete
            if pending:
                ntx += 1
            pending = []
        last_seq = bseq or last_seq
        bad = False
        doff = data_off
        blocks = []
        for i in range(1, num_blocks):
            bnum, bsz, bck = struct.unpack('<qiI', raw[16 + 16 * i:32 + 16 * i])
            if bsz <= 0 or (bnum < 0 and bnum != -1):
                bad = True
                break
            data, doff = jread(doff, bsz)
            if bnum != -1:
                if flags & BLHDR_CHECK_CHECKSUMS and bck and calc_checksum(data) != bck:
                    bad = True
                    break
                blocks.append((bnum * jhdr_size, data))
        if bad:
            stop_reason = f'bad block in block list at {off}'
            break
        pending += blocks
        nbl += 1
        off = wrap(off + bytes_used)
    else:
        committed += pending
        if pending:
            ntx += 1
        pending = []

    nbytes = sum(len(d) for _, d in committed)
    print(f'{nbl} block lists, {ntx} complete transactions, {len(committed)} blocks '
          f'({nbytes} bytes) to write; stopped: {stop_reason}'
          + (f'; {len(pending)} blocks of an incomplete transaction dropped' if pending else ''))
    if dry:
        return
    for dev_off, data in committed:
        f.seek(dev_off)
        f.write(data)
    hdr[8:16] = struct.pack('<q', end if stop_reason == 'reached end' else off)
    new_start = struct.unpack('<q', hdr[8:16])[0]
    hdr[16:24] = struct.pack('<q', new_start)   # start = end: journal empty
    tmp = bytearray(hdr[:JHDR_CKSUM_SIZE])
    tmp[36:40] = b'\0\0\0\0'
    hdr[36:40] = struct.pack('<I', calc_checksum(tmp))
    f.seek(joff)
    f.write(hdr)
    f.flush()
    print(f'replayed; journal header now start = end = {new_start}')


if __name__ == '__main__':
    main()
