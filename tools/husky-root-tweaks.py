#!/usr/bin/env python3
"""Apply the HUSKY bring-up tweaks to the iOS 26 root image (artifacts/ios26-root-23G90.img).

  sudo husky-root-tweaks.py [--dry-run] [--platform-uuid UUID] MOUNTED_ROOT

MOUNTED_ROOT is the HFSX image mounted read-write (Linux: `fsck.hfsplus -fy`
the image, then `mount -t hfsplus -o loop,force`). Run as root for a real
run (it writes root/mobile owned files); --dry-run needs neither root nor a
writable mount and prints every change instead of making it. Idempotent:
a second run reports everything as unchanged.

Adapted from ios-sweet/tools/sweet-root-tweaks.py (iOS 16.7.16 / 20H392),
each tweak re-checked against the real 23G90 / 26.6.2 image:

  launchd         /System/Library/xpc/launchd.plist is STILL the compiled job
                  cache (690 jobs, keyed by plist path; needs the boot-arg
                  launchd_unsecure_cache=1 because launchd.plist.sig no longer
                  matches once edited). Edits there: _PanicOnCrash ->
                  _PanicOnCrasX (a crashing daemon must not panic the system;
                  9 jobs on 23G90) and the com.apple.CommCenter jobs removed
                  (no baseband: crash loop, Setup blocks on CoreTelephony).
                  iOS 26 ALSO still ships the 628 individual plists in
                  /System/Library/LaunchDaemons (+ LaunchAngels etc.); they are
                  patched the same way (CommCenter plists renamed *.disabled)
                  so the two sources cannot disagree if launchd ever falls back
                  to scanning the directories. Format (binary/XML) preserved.
  containers      /private/var/containers/{Bundle,Data/System,Shared/SystemGroup,Temp}
                  + /private/var/mobile/Containers/Shared/AppGroup. Only
                  missing dirs are created; existing dirs are never touched.
                  NO per-group subdirs: containermanagerd deletes group dirs
                  that lack a metadata plist and self-provisions the rest.
  migration       LastSystemVersion=23G90 (+ DMUserDataDisposition) merged into
                  the plain com.apple.migration.plist AND every ByHost/
                  com.apple.migration.<UUID>.plist (CurrentHost writes land in
                  ByHost now that IOPlatformUUID exists). Existing keys are kept.
  audio           VirtualAudio HAL plug-in disabled (mediaserverd crash: no
                  virtual audio device) and MediaToolbox SubServerConfiguration
                  StartRecorderServer=false in every device dir (D421, D431,
                  Default).
  SpringBoard     BKNoWatchdogs, no cloud-configuration UI
  Setup           skipped: SetupDone, SetupFinishedAllSteps, SetupVersion,
                  mini-buddy flags off
  activation      mobileactivationd data ark. On iOS 26 the ark lives in the
                  system GROUP container systemgroup.com.apple.mobileactivationd
                  (or the system container com.apple.mobileactivationd); neither
                  exists on a fresh image (containermanagerd creates it at
                  runtime). If neither is present the ark is seeded at the
                  legacy path /private/var/mobile/Library/mad/data_ark.plist,
                  which mobileactivationd migrates into the container itself.
  ui              Reduce Transparency (com.apple.Accessibility
                  EnhancedBackgroundContrastEnabled) and Reduce Motion
                  (ReduceMotionEnabled), keys checked in iOS 26's
                  libAccessibility: without a GPU, CoreAnimation renders on the
                  CPU, and its backdrop/blur path (CA::OGL::AsynchronousNode)
                  asserted in backboardd (boot 0084) and is the costliest part.
  ifnamer         configd InterfaceNamer.bundle Info.plist WaitStackTimeout /
                  WaitQuietTimeout 20 s (defaults 300/240): SystemConfiguration's
                  __wait_for_IOKit_to_quiesce blocks every SCNetworkInterface
                  caller (Settings' main thread, boot 0089b) until IOKit goes
                  quiet or this timer fires, and without an IONetworkStack the
                  300 s stack timeout applies.
  timezone        /private/var/db/timezone/localtime ->
                  /var/db/timezone/zoneinfo/<--timezone> (default
                  Asia/Ho_Chi_Minh; the image ships US/Pacific).

The ui and timezone tweaks can also be applied to the phone directly, without
pulling the root back: tools/ios-tweak-ondevice.py.
"""
import argparse, glob, os, plistlib, sys

BUILD, VERSION = '23G90', '26.6.2'
# Userland builds the tweaks have been checked against (build -> version);
# BUILD/VERSION are set from the image's SystemVersion.plist.
SUPPORTED = {'23G90': '26.6.2', '23F77': '26.5'}
MOBILE = 501
SETUP_VERSION = 11          # iOS 16 value; the key still exists on iOS 26, current value unverified

DRY = False

# Shared with tools/ios-tweak-ondevice.py.
UI_PREFS = {
    'com.apple.Accessibility': {
        'EnhancedBackgroundContrastEnabled': True,     # Reduce Transparency
        'ReduceMotionEnabled': True,
    },
}
TIMEZONE = 'Asia/Ho_Chi_Minh'
# Root-owned system plists: path relative to the root -> keys merged in.
SYSTEM_PLISTS = {
    'System/Library/SystemConfiguration/InterfaceNamer.bundle/Info.plist': {
        'WaitStackTimeout': 20.0,
        'WaitQuietTimeout': 20.0,
    },
}
LOCALTIME = 'private/var/db/timezone/localtime'


def zone_target(tz):
    return '/var/db/timezone/zoneinfo/' + tz


def say(msg):
    print(('[dry-run] ' if DRY else '') + msg)


# ---------------------------------------------------------------- fs helpers

def load(path):
    with open(path, 'rb') as f:
        return plistlib.load(f)


def file_fmt(path):
    with open(path, 'rb') as f:
        return plistlib.FMT_BINARY if f.read(6) == b'bplist' else plistlib.FMT_XML


def mkdir(path, uid, gid, mode=0o755):
    """Create a missing directory (never touches an existing one)."""
    if os.path.isdir(path):
        return False
    say(f'mkdir {path} ({uid}:{gid} {mode:o})')
    if not DRY:
        os.makedirs(path)
        os.chown(path, uid, gid)
        os.chmod(path, mode)
    return True


def ensure_dir_tree(root, rel, uid, gid, mode=0o755):
    """mkdir -p with owner uid:gid on every directory that has to be created."""
    cur = root
    for part in rel.strip('/').split('/'):
        cur = os.path.join(cur, part)
        mkdir(cur, uid, gid, mode)


def write_plist(path, data, uid, gid, mode, fmt, what):
    """Write PATH if content/owner/mode differ. Atomic (tmp + rename)."""
    exists = os.path.exists(path)
    if exists:
        same = load(path) == data
        st = os.stat(path)
        if same and (st.st_uid, st.st_gid, st.st_mode & 0o7777) == (uid, gid, mode):
            say(f'unchanged {what}')
            return False
    say(f'{"update" if exists else "create"} {what}  ({uid}:{gid} {mode:o})')
    if not DRY:
        tmp = path + '.husky-tmp'
        with open(tmp, 'wb') as f:
            plistlib.dump(data, f, fmt=fmt)
        os.chown(tmp, uid, gid)
        os.chmod(tmp, mode)
        os.replace(tmp, path)
    return True


def merge_dict(old, new):
    """Recursive merge: keys in NEW win, other keys in OLD are kept."""
    out = dict(old)
    for k, v in new.items():
        out[k] = merge_dict(out[k], v) if isinstance(v, dict) and isinstance(out.get(k), dict) else v
    return out


def merge_pref_file(path, values, what, soft=()):
    """Merge VALUES into a mobile-owned pref plist. Keys in SOFT are only set
    when absent (or when `keep(old)` says the old value is stale)."""
    data = load(path) if os.path.exists(path) else {}
    fmt = file_fmt(path) if os.path.exists(path) else plistlib.FMT_BINARY
    new = merge_dict(data, {k: v for k, v in values.items() if k not in soft})
    for k in soft:
        if k in values and not (k in data and data[k].get('buildVersion') == values[k].get('buildVersion')):
            new[k] = values[k]
    st = os.stat(path) if os.path.exists(path) else None
    mode = (st.st_mode & 0o7777) if st else 0o600
    return write_plist(path, new, MOBILE, MOBILE, mode, fmt, f'{what} {sorted(values)}')


def prefs_dir(root):
    return os.path.join(root, 'private/var/mobile/Library/Preferences')


def merge_pref(root, name, values, soft=()):
    """Merge keys into /var/mobile/Library/Preferences/NAME.plist."""
    path = os.path.join(prefs_dir(root), name + '.plist')
    merge_pref_file(path, values, f'pref {name}', soft)


# ------------------------------------------------------------------- launchd

LAUNCHD_DIRS = ('System/Library/LaunchDaemons', 'System/Library/LaunchAngels',
                'System/Library/NanoLaunchDaemons', 'System/Library/NanoLaunchDaemonsAltAccount',
                'System/Library/DeveloperModeLaunchDaemons', 'Library/LaunchDaemons')


def patch_panic(job):
    if '_PanicOnCrash' in job:
        job['_PanicOnCrasX'] = job.pop('_PanicOnCrash')
        return True
    return False


def launchd(root):
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    if os.path.exists(path):
        st = os.stat(path)
        plist = load(path)
        jobs = plist['LaunchDaemons']
        renamed = sum(patch_panic(j) for j in jobs.values())
        removed = [k for k, j in jobs.items() if j.get('Label') == 'com.apple.CommCenter']
        for k in removed:
            del jobs[k]
        say(f'launchd.plist cache: {renamed} _PanicOnCrash renamed, {len(removed)} CommCenter jobs removed'
            + (''.join(f'\n    - {k}' for k in removed) if removed else ''))
        if renamed or removed:
            say('NOTE launchd.plist.sig becomes stale: boot with launchd_unsecure_cache=1')
            write_plist(path, plist, st.st_uid, st.st_gid, st.st_mode & 0o7777,
                        file_fmt(path), 'System/Library/xpc/launchd.plist')
    else:
        say('warning: no System/Library/xpc/launchd.plist; only patching individual job plists')

    # Individual job plists (still shipped on iOS 26; belt and braces).
    ren = dis = 0
    for d in LAUNCHD_DIRS:
        for f in sorted(glob.glob(os.path.join(root, d, '*.plist'))):
            with open(f, 'rb') as fh:
                raw = fh.read()
            if b'_PanicOnCrash' not in raw and b'CommCenter' not in raw:
                continue
            job = plistlib.loads(raw)
            rel = os.path.relpath(f, root)
            if job.get('Label') == 'com.apple.CommCenter':
                say(f'disable job plist {rel} -> {os.path.basename(f)}.disabled')
                dis += 1
                if not DRY:
                    os.rename(f, f + '.disabled')
            elif patch_panic(job):
                st = os.stat(f)
                write_plist(f, job, st.st_uid, st.st_gid, st.st_mode & 0o7777, file_fmt(f), rel)
                ren += 1
    say(f'job plists: {ren} _PanicOnCrash renamed, {dis} CommCenter plists disabled')


# ---------------------------------------------------------------- containers

def containers(root):
    base = os.path.join(root, 'private/var/containers')
    for sub, uid in (('Bundle', 33), ('Data/System', 0), ('Shared/SystemGroup', 0), ('Temp', 0)):
        mkdir(os.path.join(base, sub), uid, uid)
    # Needed for app-group containers; owned by mobile.
    ensure_dir_tree(root, 'private/var/mobile/Containers/Shared/AppGroup', MOBILE, MOBILE)


# ---------------------------------------------------------------- activation

def find_container(root, kind, identifier):
    for meta in glob.glob(os.path.join(root, 'private/var/containers', kind, '*',
                                       '.com.apple.mobile_container_manager.metadata.plist')):
        try:
            if load(meta).get('MCMMetadataIdentifier') == identifier:
                return os.path.dirname(meta)
        except Exception:
            pass
    return None


def ark_values():
    """Keys merged into mobileactivationd's data ark (shared with
    tools/ios-tweak-ondevice.py)."""
    return {
        '-ActivationState': 'Activated', '-BrickState': False,
        '-BuildVersion': BUILD, '-ProductVersion': VERSION,
        '-DataMigrationCompleted': True, '-PreSydroDataMigrationCompleted': True,
    }


def activation(root):
    ark = ark_values()
    # mobileactivationd reads the ark from its Data/System container
    # (container_system_path_for_identifier("com.apple.mobileactivationd") +
    # Library/internal/data_ark.plist). If -BuildVersion matches the running
    # build it skips its first-boot migration (which fails here and os_crashes
    # "Migration failed.") and Hactivation reports Activated. The legacy
    # /var/mobile/Library/mad ark only contributes -LastActivated.
    c = find_container(root, 'Data/System', 'com.apple.mobileactivationd')
    if c is None:
        say('warning: no Data/System container for com.apple.mobileactivationd in the image '
            '(containermanagerd creates it on the first boot with working XPC); boot once, '
            'pull the device image and re-run this script')
        return
    st = os.stat(c)
    d = os.path.join(c, 'Library/internal')
    mkdir(d, st.st_uid, st.st_gid)
    target = os.path.join(d, 'data_ark.plist')
    if os.path.exists(target):
        ark = merge_dict(load(target), ark)
    write_plist(target, ark, st.st_uid, st.st_gid, 0o600, plistlib.FMT_BINARY,
                'data ark ' + os.path.relpath(target, root))


# ------------------------------------------------------------------ dirs

def user_dirs(root):
    # chronod (widgets) fatalErrors when BSCurrentUserDirectory()/Library/chrono
    # cannot be created/opened.
    mkdir(os.path.join(root, 'private/var/mobile/Library/chrono'), MOBILE, MOBILE)


# --------------------------------------------------------------------- audio

def audio(root):
    src = os.path.join(root, 'Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin')
    dst_dir = os.path.join(root, 'Library/Audio/Plug-Ins/HAL.disabled')
    dst = os.path.join(dst_dir, 'VirtualAudio.plugin')
    if os.path.exists(src):
        if os.path.exists(dst):
            say('warning: HAL.disabled/VirtualAudio.plugin already exists; leaving HAL/VirtualAudio.plugin')
        else:
            say('move Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin -> HAL.disabled/')
            if not DRY:
                os.makedirs(dst_dir, exist_ok=True)
                os.rename(src, dst)
    else:
        say('unchanged VirtualAudio.plugin ' + ('(already disabled)' if os.path.exists(dst) else '(not present)'))
    confs = sorted(glob.glob(os.path.join(
        root, 'System/Library/Frameworks/MediaToolbox.framework/*/SubServerConfiguration.plist')))
    if not confs:
        say('warning: no MediaToolbox SubServerConfiguration.plist found')
    for conf in confs:
        st = os.stat(conf)
        data = load(conf)
        data['StartRecorderServer'] = False
        write_plist(conf, data, st.st_uid, st.st_gid, st.st_mode & 0o7777, file_fmt(conf),
                    'StartRecorderServer=false ' + os.path.relpath(conf, root))


# ------------------------------------------------------------------ migration

def migration(root, platform_uuid):
    vals = {'LastSystemVersion': BUILD,
            'DMUserDataDisposition': {'SyntheticDidUpgrade': False, 'buildVersion': BUILD}}
    # DMUserDataDisposition is only replaced if its buildVersion is stale
    # (datamigrator may have written a fuller one itself).
    pd = prefs_dir(root)
    merge_pref(root, 'com.apple.migration', vals, soft=('DMUserDataDisposition',))
    byhost = os.path.join(pd, 'ByHost')
    files = set(glob.glob(os.path.join(byhost, 'com.apple.migration.*.plist')))
    if platform_uuid:
        mkdir(byhost, MOBILE, MOBILE)
        for u in (platform_uuid.upper(), platform_uuid.lower()):
            files.add(os.path.join(byhost, f'com.apple.migration.{u}.plist'))
    if not files:
        say('note: no ByHost/com.apple.migration.<UUID>.plist (pass --platform-uuid to create)')
    for f in sorted(files):
        merge_pref_file(f, vals, 'pref ByHost/' + os.path.basename(f), soft=('DMUserDataDisposition',))


# ----------------------------------------------------------------------- main

def system_plists(root):
    for rel, values in SYSTEM_PLISTS.items():
        path = os.path.join(root, rel)
        if not os.path.exists(path):
            say(f'warning: no {rel}')
            continue
        st = os.stat(path)
        new = dict(load(path))
        new.update(values)
        write_plist(path, new, st.st_uid, st.st_gid, st.st_mode & 0o7777, file_fmt(path),
                    f'{rel} {sorted(values)}')


def ui(root):
    for name, values in UI_PREFS.items():
        merge_pref(root, name, values)


def timezone(root, tz):
    if not os.path.exists(os.path.join(root, 'usr/share/zoneinfo.default', tz)):
        sys.exit(f'no zone {tz} in usr/share/zoneinfo.default')
    link = os.path.join(root, LOCALTIME)
    want = zone_target(tz)
    if os.path.islink(link) and os.readlink(link) == want:
        say(f'unchanged {LOCALTIME} -> {want}')
        return
    say(f'{LOCALTIME} -> {want}')
    if not DRY:
        tmp = link + '.husky-tmp'
        if os.path.lexists(tmp):
            os.unlink(tmp)
        os.symlink(want, tmp)
        os.lchown(tmp, 0, 0)
        os.replace(tmp, link)


def main():
    global DRY
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('root', metavar='MOUNTED_ROOT')
    ap.add_argument('--dry-run', action='store_true', help='print every change, write nothing')
    ap.add_argument('--platform-uuid', help='IOPlatformUUID: create ByHost migration plists for it')
    ap.add_argument('--timezone', default=TIMEZONE, help=f'zoneinfo name (default {TIMEZONE})')
    args = ap.parse_args()
    root = args.root
    if not os.path.isdir(os.path.join(root, 'System/Library/xpc')):
        sys.exit(f'{root}: not a mounted iOS root (no System/Library/xpc)\n\n{__doc__}')
    DRY = args.dry_run
    if not DRY and os.geteuid() != 0:
        sys.exit('run as root (or use --dry-run)')
    global BUILD, VERSION
    ver = load(os.path.join(root, 'System/Library/CoreServices/SystemVersion.plist'))
    b, v = ver.get('ProductBuildVersion'), ver.get('ProductVersion')
    if SUPPORTED.get(b) != v:
        sys.exit(f'image is {v} / {b}, script supports '
                 + ', '.join(f'{sv} / {sb}' for sb, sv in SUPPORTED.items()))
    BUILD, VERSION = b, v

    launchd(root)
    containers(root)
    activation(root)
    user_dirs(root)
    audio(root)
    migration(root, args.platform_uuid)
    ui(root)
    system_plists(root)
    timezone(root, args.timezone)
    merge_pref(root, 'com.apple.springboard', {
        'BKNoWatchdogs': True,
        'FBLaunchWatchdogExceptions': {'com.apple.purplebuddy': 100.0},
        'MCForceShowCloudConfigurationUI': False})
    merge_pref(root, 'com.apple.purplebuddy', {
        'SetupDone': True, 'SetupFinishedAllSteps': True, 'SetupVersion': SETUP_VERSION,
        'daemonizeNeedsToRun': False})
    merge_pref(root, 'com.apple.purplebuddy.notbackedup', {
        k: False for k in ('ShouldRunDiagnosticsMiniBuddy', 'ShouldRunHSA2MiniBuddy',
                           'ShouldRunKeychainSyncMiniBuddy', 'ShouldRunMesaMiniBuddy',
                           'ShouldRunPaymentMiniBuddy', 'ShouldRunStandardMiniBuddy',
                           'SilentICDPUpgrade')})
    merge_pref(root, 'com.apple.managedconfiguration.notbackedup', {
        'LockdownActivationIndicatesCloudConfigurationAvailable': False})


if __name__ == '__main__':
    main()
