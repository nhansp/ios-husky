#!/usr/bin/env bash
# boot-180.sh TAG KERNELDIR USERLANDDIR [extra boot-args]: install MaliMetal (keeping the -h shader pack), boot, collect
# pmsg, huskylog and the plug-in's per-process diag files.
set -u
cd /home/nhansp/aosp/ios-husky
S=3A110DLJG0019U
SP=out/dev; mkdir -p $SP out/logs out/boot
T=tools/dev
TAG=$1 K=$2 G=$3; shift 3
P=out/gpu-userland-h/shaders.mvpack
until adb devices | grep -q "${S}[[:space:]]*device"; do sleep 5; done
sleep 5
for attempt in 1 2 3; do
	python3 -I tools/ios-tweak-ondevice.py \
		--install-file $G/MaliMetal.bundle/MaliMetal:/System/Library/Extensions/MaliMetal.bundle/MaliMetal:755 \
		--install-file $G/MaliMetal.bundle/Info.plist:/System/Library/Extensions/MaliMetal.bundle/Info.plist:644 \
		--install-file $G/MaliMetal.bundle/_CodeSignature/CodeResources:/System/Library/Extensions/MaliMetal.bundle/_CodeSignature/CodeResources:644 \
		--install-file $P:/System/Library/Extensions/MaliMetal.bundle/shaders.mvpack:644 \
		$( [ -f $G/MaliMetal.bundle/Frameworks/libMaliCompiler.dylib ] && echo --install-file $G/MaliMetal.bundle/Frameworks/libMaliCompiler.dylib:/System/Library/Extensions/MaliMetal.bundle/Frameworks/libMaliCompiler.dylib:755 ) \
		> $SP/deploy-$TAG.log 2>&1 && break
done
tail -3 $SP/deploy-$TAG.log | cut -c1-160
grep -q "not cleanly unmounted\|Traceback\|refus" $SP/deploy-$TAG.log && echo "DEPLOY FAILED: booting what is already on the volume"
CMDLINE="debug=0x10000c cpus=9 keepsyms=1 -v -enable_kprintf_spam startup_debug=1 -noprogress rd=ufsroot0 ufsrw=1 rootrw=1 launchd_unsecure_cache=1 huskygrowroot=1 huskysample=PosterBoard,CollectionsPoster,SpringBoard huskyiolog=backboardd,SpringBoard,PosterBoard,CollectionsPoster huskynull=backboardd,SpringBoard,PosterBoard,CollectionsPoster huskyfb_noconsole=1 huskytouch=1 huskytouchdelay=30 huskyreboot=180 huskyboost=1 huskygpu=7 huskygpuclients=${GPUCLIENTS:-metal_smoke,backboardd} huskygputrace=1 ufslogsecs=1 huskysurfcompressed=1 $*"
CMDLINE="$CMDLINE" AVBTOOL=$SP/avb/avbtool.py AVBKEY=$SP/avb/key4096.pem AVB_ROLLBACK=${AVB_ROLLBACK:-1791158400} \
	AVB_PROPS="com.android.build.boot.os_version:17 com.android.build.boot.security_patch:2026-10-05" bash tools/mkxnuboot.sh $K/kernel.development.husky out/boot/xnu-boot-$TAG.img --highmem >/dev/null 2>&1 || { echo "mkxnuboot failed"; exit 1; }
bash $T/wait-boot.sh out/boot/xnu-boot-$TAG.img 2>&1 | tail -1
fastboot devices | grep -q $S && { echo "ABORT: still in fastboot"; exit 1; }
sleep 185
until adb devices | grep -q "${S}[[:space:]]*device"; do sleep 5; done
sleep 5
echo "back, reason $(adb -s $S shell getprop ro.boot.bootreason | tr -d '\r')"
adb -s $S shell 'su -c "cat /sys/fs/pstore/pmsg-ramoops-0"' 2>/dev/null | strings > out/logs/pmsg-$TAG.txt
bash tools/husky-log-pull.sh out/logs/boot-$TAG.txt 2>&1 | tail -1
grep -a -m1 -A2 "HUSKY PANIC" out/logs/pmsg-$TAG.txt | sed 's/^/PANIC: /'
for f in out/logs/boot-*.txt; do [ "$f" != out/logs/boot-$TAG.txt ] && cmp -s "$f" out/logs/boot-$TAG.txt && echo "STALE huskylog: identical to $f"; done
D=out/logs/diag-$TAG; mkdir -p $D
bash tools/ios-mount.sh >/dev/null 2>&1
adb -s $S exec-out "su -c 'cd /data/local/tmp/ios/private/var/tmp && tar -cf - mali-*.log malimetal'" 2>/dev/null | tar -xf - -C $D 2>/dev/null
adb -s $S shell "su -c 'rm -f /data/local/tmp/ios/private/var/tmp/mali-*.log'" >/dev/null 2>&1
bash tools/ios-mount.sh -u >/dev/null 2>&1
grep -h "METAL-SMOKE" out/logs/boot-$TAG.txt | cut -c1-160
grep -ah "on-device compiler\|UNSUPPORTED\|HUSKY IOGPU bridge: CSG.*blocked\|TIMED OUT\|IOGPU: import\|IOGPU: client-memory\|NEW_RESOURCE refused\|CRASH backboardd reason" out/logs/boot-$TAG.txt | sort | uniq -c | sort -rn | head -12 | cut -c1-400