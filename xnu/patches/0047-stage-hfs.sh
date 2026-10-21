#!/bin/bash
# dxb runs patches/*.sh from the XNU source root before applying *.patch.
# Stages Apple's open-source HFS for the built-in HUSKY filesystem
# (0047-husky-builtin-hfs.patch, bsd/husky_hfs); ported from ios-sweet 0026.
set -euo pipefail

[[ "${MACHINE_CONFIG:-}" == HUSKY ]] || exit 0
[[ "${MACOS_VERSION:-}" == 26.* ]] || {
    echo 'HUSKY HFS integration requires xnu-12377 (macOS 26.x)' >&2
    exit 1
}
[[ -f bsd/kern/bsd_init.c ]] || exit 1

# hfs-715.120.4 is paired with xnu-12377.121.6 in Apple's macos-265
# release.json. Fetch the immutable commit, never a moving branch/tag.
hfs_sha=124b970fd5390f1c2cd6f985478616a04f6eeb1f
hfs_tmp=$(mktemp -d)
trap 'rm -rf "$hfs_tmp"' EXIT
git init -q "$hfs_tmp"
git -C "$hfs_tmp" fetch -q --depth 1 \
    https://github.com/apple-oss-distributions/hfs "$hfs_sha"
git -C "$hfs_tmp" checkout -q FETCH_HEAD
[[ "$(git -C "$hfs_tmp" rev-parse HEAD)" == "$hfs_sha" ]]

python3 - "$hfs_tmp" <<'PY'
from pathlib import Path
import shutil
import sys

source = Path(sys.argv[1])
target = Path('bsd/husky_hfs')
if target.exists():
    raise SystemExit(f'refusing to overwrite existing HFS sources: {target}')
# Preserve upstream copyright/license headers; no downloaded sources in git.
for directory in ('core', 'hfs_encodings'):
    shutil.copytree(source / directory, target / directory)

def replace(file, old, new, count=1):
    path = target / file
    text = path.read_text()
    if text.count(old) != count:
        raise SystemExit(f'HFS source changed: {path}: {old!r}')
    path.write_text(text.replace(old, new))

# HUSKY has no AppleKeyStore (content protection is not built, see
# husky_hfs_compat.h); its header is only needed for the key-wrapping API.
path = target / 'core' / 'hfs_iokit.h'
text = path.read_text()
include = '#include <AppleKeyStore/AppleKeyStoreFSServices.h>\n'
first = 'int hfs_unwrap_key('
end = '__END_DECLS'
if text.count(include) != 1 or text.count(first) != 1 or text.count(end) != 1:
    raise SystemExit(f'HFS source changed: {path}')
text = text.replace(include, '#if CONFIG_PROTECT\n' + include + '#endif\n')
i, j = text.index(first), text.index(end)
text = text[:i] + '#if CONFIG_PROTECT\n' + text[i:j].rstrip('\n') + '\n#endif\n\n' + text[j:]
path.write_text(text)

# HUSKY never unmounts its root (no clean shutdown yet) and runs no fsck_hfs
# before the root mount: take a dirty non-journaled root read-write anyway
# (ios-sweet 0029). Other volumes keep the upstream refusal.
replace('core/hfs_vfsutils.c',
        '''		if (HFS_MOUNT_DEBUG) {
			printf("hfs_mounthfsplus: cannot mount dirty non-journaled volumes\\n");
		}
		return (EINVAL);''',
        '''		if (vfs_flags(hfsmp->hfs_mp) & MNT_ROOTFS) {
			printf("hfs_mounthfsplus: HUSKY: mounting dirty non-journaled root read-write\\n");
		} else {
		if (HFS_MOUNT_DEBUG) {
			printf("hfs_mounthfsplus: cannot mount dirty non-journaled volumes\\n");
		}
		return (EINVAL);
		}''')

# Hard links made by Linux tools lack kHFSHasLinkChainBit; every lookup
# repairs it in memory and prints again, flooding the console and slowing
# boot. Report the first 8 only (ios-sweet 0041).
replace('core/hfs_catalog.c', """/*
 * cat_lookupbykey - lookup a catalog node using a cnode key
 */""", """/* HUSKY: the link-repair messages below, the first few only. */
static int
husky_hfs_link_note(void)
{
	static volatile SInt32 notes;
	return OSIncrementAtomic(&notes) < 8;
}

/*
 * cat_lookupbykey - lookup a catalog node using a cnode key
 */""")
for msg in ('printf ("hfs: set hardlink bit on vol=%s cnid=%u inoid=%u\\n", hfsmp->vcbVN, cnid, ilink);',
            'printf ("hfs: clear hardlink bit on vol=%s cnid=%u\\n", hfsmp->vcbVN, cnid);',
            'printf ("hfs: set linkcount=1 on vol=%s cnid=%u old=%u\\n", hfsmp->vcbVN, cnid, attrp->ca_linkcount);'):
    replace('core/hfs_catalog.c', msg, 'if (husky_hfs_link_note()) ' + msg)
PY

# Kernel builds use -nostdinc; HFS's kext-config.h needs the SDK's target macros.
hfs_sdk=$(xcrun --sdk macosx --show-sdk-path)
cp "$hfs_sdk/usr/include/TargetConditionals.h" bsd/husky_hfs/
echo "Staged hfs-715.120.4 ($hfs_sha) for HUSKY"
