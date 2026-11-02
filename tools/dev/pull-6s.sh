#!/usr/bin/env bash
# pull-6s.sh OUTDIR [AFTER]: pull 6s DCIM photos named after AFTER (default IMG_0020.JPG, the last unrelated photo).
# Filters by name, not EXIF time: the 6s is set to US/Pacific.
set -u
PMD=${PMD:-/home/nhansp/aosp/ios-husky/out/dev/pmd/bin/pymobiledevice3}; O=$1; AFTER=${2:-IMG_0020.JPG}
mkdir -p "$O"
for f in $($PMD afc ls /DCIM 2>/dev/null | grep -E '/[0-9]{3}APPLE$'); do
	for p in $($PMD afc ls "$f" 2>/dev/null | grep -iE '\.(jpg|heic|png|mov)$'); do
		n=$(basename "$p")
		[[ "$f/$n" > "/DCIM/100APPLE/$AFTER" ]] || continue
		[ -f "$O/$n" ] || { $PMD afc pull "$p" "$O/" >/dev/null 2>&1 && echo "pulled $n"; }
	done
done
