#!/usr/bin/env bash
# Wait for husky in adb or fastboot, then fastboot-boot the given image.
IMG=$1; S=3A110DLJG0019U
for i in $(seq 1 1800); do
  if fastboot devices | grep -q $S; then break; fi
  if adb devices | grep -q "$S[[:space:]]*device"; then adb -s $S reboot bootloader; sleep 5; fi
  sleep 2
done
fastboot devices | grep -q $S || { echo "TIMEOUT: no device"; exit 1; }
for try in 1 2; do
  fastboot -s $S boot "$IMG" >/dev/null 2>&1 || continue
  sleep 10
  # ABL rejects an image that fails AVB by staying in fastboot (looks like Booting OKAY)
  fastboot devices | grep -q $S || { echo "BOOTED $IMG at $(date +%T)"; exit 0; }
  echo "still in fastboot after fastboot boot (try $try)"
done
exit 1
