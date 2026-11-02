#!/usr/bin/env bash
# dl-artifact.sh RUN_ID NAME OUTDIR: resumable artifact download (gh run download stalls on slow blob storage).
set -euo pipefail
R=$1 N=$2 O=$3
SP=/home/nhansp/aosp/ios-husky/out/dev; mkdir -p $SP
cd /home/nhansp/aosp/ios-husky
ID=$(gh api repos/nhansp/ios-husky/actions/runs/$R/artifacts --jq ".artifacts[] | select(.name==\"$N\") | .id")
URL=$(curl -sI -H "Authorization: Bearer $(gh auth token)" https://api.github.com/repos/nhansp/ios-husky/actions/artifacts/$ID/zip | grep -i '^location:' | cut -d' ' -f2 | tr -d '\r')
Z=$SP/$N-$R.zip
for i in $(seq 1 20); do curl -L -C - --http1.1 -s -S --max-time 100 -o "$Z" "$URL" && break; done
rm -rf "$O" && mkdir -p "$O" && unzip -q "$Z" -d "$O" && rm -f "$Z"
[ -f "$O/SHA256SUMS" ] && (cd "$O" && sha256sum -c SHA256SUMS | tail -1)
echo "downloaded $N to $O"
