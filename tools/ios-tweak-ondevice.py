#!/usr/bin/env python3
"""Apply small iOS root tweaks (UI prefs, system plists, timezone, activation ark) to husky over adb.

  tools/ios-tweak-ondevice.py [--dry-run] [--timezone NAME]

The full image round trip (pull-ios-root-delta.sh, hfs-journal-replay.py,
husky-root-tweaks.py, flash-ios-root.sh) no longer fits: xnu 0075 grew the
volume to the whole UFS window (~70 GiB), and the window past the old 20 GiB
still holds stale Android data, so nearly every chunk would differ. This edits
the live volume from rooted Android instead, with the same out-of-tree
hfsplus.ko as tools/ios-mount.sh, mounted read-write through a dm-linear
mapping of the iOS window.

Only on a CLEAN volume: the HFS+ "unmounted" attribute set and the journal
empty (start == end), as after a clean xnu reboot (0086, boot-arg
huskyreboot=N). Linux's hfsplus ignores the journal, so after a force-reboot
XNU could replay stale transactions over the edits; the tool refuses then.

The tweak values come from tools/husky-root-tweaks.py (UI_PREFS, TIMEZONE),
so both paths apply the same thing. Plists are merged on the host and written
back with their owner and mode; the timezone is the localtime symlink.
"""
import argparse, importlib.util, io, os, plistlib, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SERIAL = os.environ.get('SERIAL', '3A110DLJG0019U')
DEV = '/dev/block/by-name/userdata'
ROOT_OFFSET = 40 << 30          # ADT /husky-ufs root-offset
LOG_SIZE = 64 << 20             # ADT /husky-ufs log-size
MNT = '/data/local/tmp/iosrw'
KO = os.environ.get('KO', os.path.join(HERE, '../artifacts/gki-hfsplus/src/fs/hfsplus/hfsplus.ko'))
JNL_MAGIC, ENDIAN_MAGIC = 0x4a4e4c78, 0x12345678
JHDR_CKSUM_SIZE = 44

spec = importlib.util.spec_from_file_location('tweaks', os.path.join(HERE, 'husky-root-tweaks.py'))
tweaks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tweaks)


def adb(*args, data=None):
    r = subprocess.run(['adb', '-s', SERIAL, *args], input=data, capture_output=True)
    if r.returncode:
        sys.exit(f'adb {" ".join(args)}: {r.stderr.decode(errors="replace").strip()}')
    return r.stdout


def su(cmd):
    return adb('shell', f"su -c '{cmd}'").decode(errors='replace').replace('\r', '')


def su_bytes(cmd):
    return adb('exec-out', f"su -c '{cmd}'")


def read_vol(off, n):
    """n bytes at byte offset off of the iOS volume (4 KiB aligned reads)."""
    first = (ROOT_OFFSET + off) // 4096
    count = ((ROOT_OFFSET + off + n + 4095) // 4096) - first
    raw = su_bytes(f'dd if={DEV} bs=4096 skip={first} count={count} 2>/dev/null')
    start = (ROOT_OFFSET + off) - first * 4096
    return raw[start:start + n]


def calc_checksum(data):
    c = 0
    for b in data:
        c = ((c << 8) ^ (c + b)) & 0xffffffff
    return (~c) & 0xffffffff


def check_clean():
    vh = read_vol(1024, 512)
    sig, _ver, attr, lmv, jib = struct.unpack('>2sHIII', vh[:16])
    bsize = struct.unpack('>I', vh[40:44])[0]
    if sig not in (b'H+', b'HX'):
        sys.exit(f'no HFS+ volume at the iOS window (signature {sig!r})')
    print(f'volume {sig.decode()}: attributes {attr:#x}, last mounted by {struct.pack(">I", lmv)!r}, '
          f'block size {bsize}')
    if not attr & 0x100:
        sys.exit('volume not cleanly unmounted (force-reboot?): boot xnu with huskyreboot=N and let '
                 'it reboot itself, or use the image round trip with hfs-journal-replay.py')
    if attr & 0x2000:
        ji = read_vol(jib * bsize, 64)
        jflags, = struct.unpack('>I', ji[:4])
        joff, _jsize = struct.unpack('>QQ', ji[36:52])
        if jflags & 0x2:
            sys.exit('journal on another device: not supported')
        hdr = read_vol(joff, 512)
        magic, endian, start, end, size, _bl, hcksum, jhdr_size, seq = \
            struct.unpack('<iiqqqiIiI', hdr[:48])
        if magic != JNL_MAGIC or endian != ENDIAN_MAGIC:
            sys.exit(f'unexpected journal header magic {magic:#x} endian {endian:#x}')
        tmp = bytearray(hdr[:JHDR_CKSUM_SIZE])
        tmp[36:40] = b'\0\0\0\0'
        if calc_checksum(tmp) != hcksum:
            sys.exit('journal header checksum mismatch')
        start = jhdr_size if start == size else start
        end = jhdr_size if end == size else end
        print(f'journal: start {start} end {end} seq {seq}')
        if start != end:
            sys.exit('journal not empty: refusing (XNU would replay it over the edits)')


def mount_rw(ro=False):
    if not os.path.isfile(KO):
        sys.exit(f'no {KO} (see tools/ios-mount.sh)')
    adb('push', KO, '/data/local/tmp/hfsplus.ko')
    size = int(su(f'blockdev --getsize64 {DEV}').strip())
    sectors = (size - ROOT_OFFSET - LOG_SIZE) // 512
    # Never two mounts of one volume: drop tools/ios-mount.sh's read-only one.
    su('umount /data/local/tmp/ios 2>/dev/null; dmctl delete iosroot 2>/dev/null; true')
    out = su(f'grep -q hfsplus /proc/filesystems || insmod /data/local/tmp/hfsplus.ko; '
             f'dmctl getpath iosrw >/dev/null 2>&1 || dmctl create iosrw {"-ro " if ro else ""}linear 0 {sectors} {DEV} '
             f'{ROOT_OFFSET // 512}; mkdir -p {MNT}; grep -q " {MNT} " /proc/mounts || '
             f'mount -t hfsplus -o {"ro" if ro else "rw,force"} $(dmctl getpath iosrw) {MNT}; '
             f'grep " {MNT} " /proc/mounts')
    if f' {MNT} hfsplus {"ro" if ro else "rw"}' not in out:
        unmount()
        sys.exit(f'read-write mount failed: {out.strip()}')
    print(f'mounted {"read-only (dry run)" if ro else "read-write"} at {MNT}')


def unmount():
    """Unmount and drop the mapping; retried (a umount right after writes can
    report busy) and verified, never left mounted for the next xnu boot."""
    out = su(f'sync; for i in 1 2 3 4 5 6 7 8 9 10; do grep -q " {MNT} " /proc/mounts || break; '
             f'umount {MNT} 2>/dev/null || sleep 1; done; grep " {MNT} " /proc/mounts; '
             f'dmctl delete iosrw >/dev/null 2>&1; true')
    if MNT in out:
        sys.exit(f'STILL MOUNTED, unmount {MNT} by hand before booting xnu: {out.strip()}')
    print('unmounted')


def merge_pref(name, values, dry):
    merge_plist(f'{MNT}/private/var/mobile/Library/Preferences/{name}.plist', name, values, dry)


def merge_plist(path, name, values, dry):
    exists = su(f'test -f {path} && echo yes').strip() == 'yes'
    if exists:
        raw = su_bytes(f'cat {path}')
        fmt = plistlib.FMT_BINARY if raw[:6] == b'bplist' else plistlib.FMT_XML
        old = plistlib.loads(raw)
        uid, gid, mode = su(f'stat -c "%u %g %a" {path}').split()
    else:
        fmt, old, uid, gid, mode = plistlib.FMT_BINARY, {}, str(tweaks.MOBILE), str(tweaks.MOBILE), '600'
    new = dict(old)
    new.update(values)
    if new == old:
        print(f'unchanged {name} {sorted(values)}')
        return
    print(f'{"update" if exists else "create"} {name} {sorted(values)} ({uid}:{gid} {mode})')
    if dry:
        return
    buf = io.BytesIO()
    plistlib.dump(new, buf, fmt=fmt)
    tmp = path + '.husky-tmp'
    adb('exec-in', f"su -c 'cat > {tmp}'", data=buf.getvalue())
    su(f'chown {uid}:{gid} {tmp} && chmod {mode} {tmp} && mv -f {tmp} {path}')
    if plistlib.loads(su_bytes(f'cat {path}')) != new:
        sys.exit(f'{name}: read-back mismatch')


def set_timezone(tz, dry):
    link = f'{MNT}/{tweaks.LOCALTIME}'
    want = tweaks.zone_target(tz)
    if su(f'test -f {MNT}/usr/share/zoneinfo.default/{tz} && echo yes').strip() != 'yes':
        sys.exit(f'no zone {tz} on the device image')
    cur = su(f'readlink {link}').strip()
    if cur == want:
        print(f'unchanged {tweaks.LOCALTIME} -> {want}')
        return
    print(f'{tweaks.LOCALTIME}: {cur} -> {want}')
    if not dry:
        su(f'ln -sfn {want} {link}')
        if su(f'readlink {link}').strip() != want:
            sys.exit('timezone link read-back mismatch')


def set_build():
    """Point the shared tweak values at the build on the device volume."""
    ver = plistlib.loads(su_bytes(f'cat {MNT}/System/Library/CoreServices/SystemVersion.plist'))
    b, v = ver.get('ProductBuildVersion'), ver.get('ProductVersion')
    if tweaks.SUPPORTED.get(b) != v:
        sys.exit(f'device volume is {v} / {b}: not a build the tweaks support')
    tweaks.BUILD, tweaks.VERSION = b, v
    print(f'device volume: iOS {v} ({b})')


def seed_ark(dry):
    """Merge the activation keys into mobileactivationd's data ark. The ark
    lives in its Data/System container, which containermanagerd creates on
    the first boot, so this only works after one boot. The file is rewritten
    in place (same catalog record), keeping the owner and mode iOS gave it."""
    base = f'{MNT}/private/var/containers/Data/System'
    found = su(f'for m in {base}/*/.com.apple.mobile_container_manager.metadata.plist; do '
               f'grep -q com.apple.mobileactivationd "$m" && dirname "$m"; done; true').split()
    if len(found) != 1:
        print(f'warning: {len(found)} mobileactivationd containers found; ark not seeded '
              f'(boot once so containermanagerd creates it)')
        return
    path = f'{found[0]}/Library/internal/data_ark.plist'
    if su(f'test -f {path} && echo yes').strip() != 'yes':
        print(f'warning: no {path} yet; ark not seeded')
        return
    raw = su_bytes(f'cat {path}')
    old = plistlib.loads(raw)
    new = dict(old)
    new.update(tweaks.ark_values())
    if new == old:
        print('unchanged data ark')
        return
    print(f'update data ark {os.path.relpath(path, MNT)} '
          f'{sorted(k for k in new if old.get(k) != new[k])}')
    if dry:
        return
    buf = io.BytesIO()
    plistlib.dump(new, buf, fmt=plistlib.FMT_BINARY if raw[:6] == b'bplist' else plistlib.FMT_XML)
    adb('exec-in', f"su -c 'cat > {path}'", data=buf.getvalue())
    if plistlib.loads(su_bytes(f'cat {path}')) != new:
        sys.exit('data ark: read-back mismatch')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--dry-run', action='store_true', help='mount read-only and report, write nothing')
    ap.add_argument('--timezone', default=tweaks.TIMEZONE)
    args = ap.parse_args()
    check_clean()
    # New files on the unlabeled hfsplus mount fail SELinux "associate", even
    # for KernelSU root: permissive only while writing, Enforcing restored.
    enforcing = not args.dry_run and su('getenforce').strip() == 'Enforcing'
    if enforcing:
        su('setenforce 0')
        print('SELinux permissive while writing')
    try:
        mount_rw(ro=args.dry_run)
        try:
            set_build()
            seed_ark(args.dry_run)
            for name, values in tweaks.UI_PREFS.items():
                merge_pref(name, values, args.dry_run)
            for rel, values in tweaks.SYSTEM_PLISTS.items():
                if su(f'test -f {MNT}/{rel} && echo yes').strip() != 'yes':
                    print(f'warning: no {rel}')
                    continue
                merge_plist(f'{MNT}/{rel}', rel, values, args.dry_run)
            set_timezone(args.timezone, args.dry_run)
        finally:
            unmount()
    finally:
        if enforcing:
            su('setenforce 1')
            print(f'SELinux {su("getenforce").strip()}')


if __name__ == '__main__':
    main()
