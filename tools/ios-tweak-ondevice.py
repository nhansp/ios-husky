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
    exists = su(f'test -f {path} && echo yes; true').strip() == 'yes'
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
    for attempt in range(3):            # a read right after the write occasionally comes back short
        try:
            if plistlib.loads(su_bytes(f'cat {path}')) == new:
                return
        except plistlib.InvalidFileException:
            pass
    sys.exit(f'{name}: read-back mismatch')


def set_timezone(tz, dry):
    link = f'{MNT}/{tweaks.LOCALTIME}'
    want = tweaks.zone_target(tz)
    if su(f'test -f {MNT}/usr/share/zoneinfo.default/{tz} && echo yes; true').strip() != 'yes':
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
    if su(f'test -f {path} && echo yes; true').strip() != 'yes':
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


MG_CACHE = ('private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/'
            'Library/Caches/com.apple.MobileGestalt.plist')


def drop_mg_cache(dry):
    """Delete MobileGestalt's answer cache so identity answers (DeviceClass,
    ProductType, ...) are recomputed after a kernel/ADT identity change."""
    path = f'{MNT}/{MG_CACHE}'
    if su(f'test -f {path} && echo yes; true').strip() != 'yes':
        print('no MobileGestalt cache')
        return
    print(f'delete {MG_CACHE}')
    if not dry:
        su(f'rm -f {path}')
        if su(f'test -e {path} && echo yes; true').strip() == 'yes':
            sys.exit('MobileGestalt cache still present')


def rerun_migration(dry):
    """Delete the com.apple.migration preference files (plain and ByHost) so iOS runs its first-boot
    data migration again. On iOS 26 that migration installs the stock apps from
    /private/var/staged_system_apps (Safari, Messages, Camera, ...); seeding LastSystemVersion made
    iOS skip it, so only the apps in /Applications (Settings, Phone) ever appeared."""
    pd = f'{MNT}/private/var/mobile/Library/Preferences'
    files = su(f'ls {pd}/com.apple.migration.plist {pd}/ByHost/com.apple.migration.*.plist 2>/dev/null; true').split()
    if not files:
        print('no migration preferences: migration will run')
        return
    for f in files:
        print(f'delete {os.path.relpath(f, MNT)}')
        if not dry:
            su(f'rm -f {f}')
    if not dry and su(f'ls {pd}/com.apple.migration.plist {pd}/ByHost/com.apple.migration.*.plist 2>/dev/null; true').split():
        sys.exit('migration preferences still present')


# IOPlatformUUID our kernel derives for this phone (ADT unique-chip-id, xnu 0048); stable across boots.
PLATFORM_UUID = '7DB69C72-B7CC-5CBD-843D-53F8F9396228'


def skip_migration(dry):
    """The reverse of rerun_migration: record the first-boot data migration as done for this build
    (husky-root-tweaks.py migration()), so iOS stops starting the ~26-minute migration (Apple logo +
    progress bar) on every boot. Only once the staged system apps are parked or installed."""
    vals = {'LastSystemVersion': tweaks.BUILD}
    disp = {'SyntheticDidUpgrade': False, 'buildVersion': tweaks.BUILD}
    pd = f'{MNT}/private/var/mobile/Library/Preferences'
    byhost = f'{pd}/ByHost'
    files = [f'{pd}/com.apple.migration.plist']
    files += su(f'ls {byhost}/com.apple.migration.*.plist 2>/dev/null; true').split()
    for u in (PLATFORM_UUID.upper(), PLATFORM_UUID.lower()):
        f = f'{byhost}/com.apple.migration.{u}.plist'
        if f not in files:
            files.append(f)
    if not dry:
        su(f'mkdir -p {byhost} && chown {tweaks.MOBILE}:{tweaks.MOBILE} {byhost}')
    for f in files:
        cur = {}
        if su(f'test -f {f} && echo yes; true').strip() == 'yes':
            cur = plistlib.loads(su_bytes(f'cat {f}'))
        v = dict(vals)
        # keep a fuller DMUserDataDisposition datamigrator wrote itself, unless it names another build
        if (cur.get('DMUserDataDisposition') or {}).get('buildVersion') != tweaks.BUILD:
            v['DMUserDataDisposition'] = disp
        merge_plist(f, os.path.relpath(f, MNT), v, dry)


def install_system_apps(tar, dry):
    """Restore the staged system-app bundles and set up the migrator to install them.

    iOS 26's MISystemAppMigrator (a DataMigrator plugin) installs the ~49 stock apps from
    /private/var/staged_system_apps into Bundle/Application on the first-boot data migration.
    On our first attempt the apps were installed and then demoted+uninstalled because they are
    not in the trust cache, leaving the staged dir empty. The migrator skips that demotion when
    <installd>/SystemAppMigratorIgnoreAppsNotInTrustCache exists (string in InstalledContentLibrary),
    which is the one ingredient missing before. This restores the (consumed) staged bundles from
    the host tar, creates that sentinel, resets the installd system-app state, and deletes the
    migration preferences so the migrator runs again on the next boot."""
    staged = f'{MNT}/private/var/staged_system_apps'
    n = len([x for x in su(f'ls {staged} 2>/dev/null; true').split() if x.endswith('.app')])
    print(f'staged_system_apps currently holds {n} .app bundles')
    sentinel = f'{MNT}/private/var/installd/SystemAppMigratorIgnoreAppsNotInTrustCache'
    mi = f'{MNT}/private/var/installd/Library/MobileInstallation'
    state_plists = ['SystemAppInstallState.plist', 'BackedUpState/SystemAppInstallState.plist',
                    'LastBuildInfo.plist', 'MigrationInfo.plist']
    if dry:
        print(f"[dry-run] would extract {tar} into private/var "
              f'({os.path.getsize(tar) // (1<<20)} MiB), chown 501:501')
        print(f'[dry-run] would touch {os.path.relpath(sentinel, MNT)}')
        for p in state_plists:
            print(f'[dry-run] would delete installd .../{p}')
        return
    # Restore the bundles: remove the emptied dir, stream the tar straight into the volume.
    su(f'rm -rf {staged}')
    rc = subprocess.run(['adb', '-s', SERIAL, 'exec-in',
                         f"su -c 'tar --numeric-owner -C {MNT}/private/var -xf -'"],
                        stdin=open(tar, 'rb')).returncode
    if rc != 0:
        sys.exit(f'tar extract failed (rc {rc})')
    su(f'chown -R 501:501 {staged}')
    n2 = len([x for x in su(f'ls {staged} 2>/dev/null; true').split() if x.endswith('.app')])
    print(f'restored {n2} .app bundles into staged_system_apps')
    if n2 < 40:
        sys.exit('restore looks short; aborting')
    su(f'install -o 501 -g 501 -m 644 /dev/null {sentinel} 2>/dev/null || (touch {sentinel}; chown 501:501 {sentinel})')
    print(f'touched {os.path.relpath(sentinel, MNT)}')
    for p in state_plists:
        su(f'rm -f {mi}/{p}')
        print(f'deleted installd .../{p}')


def install_file(spec, dry):
    """Copy a host file onto the volume: SRC:DEST[:MODE], DEST absolute on the iOS root, owned
    root:wheel (test tools and their LaunchDaemon plists, e.g. /usr/local/bin/iogpu_smoke and
    /Library/LaunchDaemons/com.husky.iogpu-smoke.plist)."""
    parts = spec.split(':')
    if len(parts) not in (2, 3) or not parts[1].startswith('/'):
        sys.exit(f'--install-file wants SRC:/DEST[:MODE], got {spec}')
    src, dest = parts[0], parts[1]
    mode = parts[2] if len(parts) == 3 else ('755' if os.access(src, os.X_OK) else '644')
    if not os.path.isfile(src):
        sys.exit(f'no such file: {src}')
    target = f'{MNT}{dest}'
    if dry:
        print(f'[dry-run] would install {src} -> {dest} (mode {mode}, root:wheel)')
        return
    su(f'mkdir -p {os.path.dirname(target)}')
    rc = subprocess.run(['adb', '-s', SERIAL, 'exec-in', f"su -c 'cat > {target}'"],
                        stdin=open(src, 'rb')).returncode
    if rc != 0:
        sys.exit(f'copy to {dest} failed (rc {rc})')
    su(f'chown 0:0 {target}; chmod {mode} {target}')
    got = su(f'wc -c < {target}').strip()
    if got != str(os.path.getsize(src)):
        sys.exit(f'{dest}: wrote {got} bytes, expected {os.path.getsize(src)}')
    print(f'installed {dest} ({got} bytes, mode {mode})')


LAUNCHD_CACHE = 'System/Library/xpc/launchd.plist'


def edit_launchd_cache(key, job, dry):
    """Add (job) or remove (job None) one entry of the compiled launchd job cache. iOS launchd runs
    jobs from this cache, keyed by plist path, not by scanning /Library/LaunchDaemons; an edited cache
    needs the boot-arg launchd_unsecure_cache=1 (husky-root-tweaks.py launchd())."""
    path = f'{MNT}/{LAUNCHD_CACHE}'
    raw = su_bytes(f'cat {path}')
    fmt = plistlib.FMT_BINARY if raw[:6] == b'bplist' else plistlib.FMT_XML
    plist = plistlib.loads(raw)
    jobs = plist['LaunchDaemons']
    if jobs.get(key) == job:
        print(f'unchanged launchd cache entry {key} ({len(jobs)} jobs)')
        return
    if job is None:
        del jobs[key]
    else:
        jobs[key] = job
    print(f'{"remove" if job is None else "set"} launchd cache entry {key} ({len(jobs)} jobs)')
    if dry:
        return
    uid, gid, mode = su(f'stat -c "%u %g %a" {path}').split()
    buf = io.BytesIO()
    plistlib.dump(plist, buf, fmt=fmt)
    tmp = path + '.husky-tmp'
    adb('exec-in', f"su -c 'cat > {tmp}'", data=buf.getvalue())
    su(f'chown {uid}:{gid} {tmp} && chmod {mode} {tmp} && mv -f {tmp} {path}')
    for attempt in range(3):            # large reads over adb occasionally come back short
        try:
            if plistlib.loads(su_bytes(f'cat {path}'))['LaunchDaemons'].get(key) == job:
                return
        except plistlib.InvalidFileException:
            pass
    sys.exit(f'{LAUNCHD_CACHE}: read-back mismatch for {key}')


def remove_file(dest, dry):
    if not dest.startswith('/') or '..' in dest:
        sys.exit(f'--remove-file wants an absolute path, got {dest}')
    if dry:
        print(f'[dry-run] would remove {dest}')
        return
    su(f'rm -f {MNT}{dest}')
    print(f'removed {dest}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--dry-run', action='store_true', help='mount read-only and report, write nothing')
    ap.add_argument('--timezone', default=tweaks.TIMEZONE)
    ap.add_argument('--rerun-migration', action='store_true',
                    help='delete the migration preferences so the first-boot migration (staged apps) runs')
    ap.add_argument('--install-system-apps', metavar='TAR',
                    help='restore staged_system_apps from TAR, set the trust-cache sentinel, reset installd '
                    'state; implies --rerun-migration so the migrator installs them next boot')
    ap.add_argument('--skip-migration', action='store_true',
                    help='record the first-boot data migration as done for this build (stops the per-boot '
                    'migration progress bar; the reverse of --rerun-migration)')
    ap.add_argument('--drop-mg-cache', action='store_true',
                    help='delete the MobileGestalt cache (after an identity change)')
    ap.add_argument('--install-file', action='append', default=[], metavar='SRC:/DEST[:MODE]',
                    help='copy a host file onto the iOS root, root:wheel (repeatable)')
    ap.add_argument('--remove-file', action='append', default=[], metavar='/DEST',
                    help='delete a file from the iOS root (repeatable)')
    ap.add_argument('--add-launchd-job', action='append', default=[], metavar='JOB.plist:/KEY',
                    help='add a job to the launchd cache, keyed by its plist path on the device (repeatable)')
    ap.add_argument('--remove-launchd-job', action='append', default=[], metavar='/KEY',
                    help='remove a job from the launchd cache (repeatable)')
    args = ap.parse_args()
    if args.skip_migration and (args.rerun_migration or args.install_system_apps):
        sys.exit('--skip-migration contradicts --rerun-migration / --install-system-apps')
    if args.install_system_apps:
        args.rerun_migration = True
        if not os.path.isfile(args.install_system_apps):
            sys.exit(f'no such tar: {args.install_system_apps}')
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
                if su(f'test -f {MNT}/{rel} && echo yes; true').strip() != 'yes':
                    print(f'warning: no {rel}')
                    continue
                merge_plist(f'{MNT}/{rel}', rel, values, args.dry_run)
            set_timezone(args.timezone, args.dry_run)
            if args.install_system_apps:
                install_system_apps(args.install_system_apps, args.dry_run)
            if args.drop_mg_cache:
                drop_mg_cache(args.dry_run)
            if args.rerun_migration:
                rerun_migration(args.dry_run)
            if args.skip_migration:
                skip_migration(args.dry_run)
            for spec in args.install_file:
                install_file(spec, args.dry_run)
            for dest in args.remove_file:
                remove_file(dest, args.dry_run)
            for spec in args.add_launchd_job:
                src, _, key = spec.partition(':')
                if not key.startswith('/'):
                    sys.exit(f'--add-launchd-job wants JOB.plist:/KEY, got {spec}')
                with open(src, 'rb') as fh:
                    edit_launchd_cache(key, plistlib.load(fh), args.dry_run)
            for key in args.remove_launchd_job:
                edit_launchd_cache(key, None, args.dry_run)
        finally:
            unmount()
    finally:
        if enforcing:
            su('setenforce 1')
            print(f'SELinux {su("getenforce").strip()}')


if __name__ == '__main__':
    main()
