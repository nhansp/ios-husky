#!/usr/bin/env bash
# push-ci.sh WORKFLOW [inputs...]: push HEAD (retrying), dispatch WORKFLOW on that exact commit, wait,
# print the conclusion and every compile/link error.
set -u
WF=$1; shift
SP=/home/nhansp/aosp/ios-husky/out/dev; mkdir -p $SP
cd /home/nhansp/aosp/ios-husky
HEAD=$(git rev-parse HEAD)
for i in $(seq 1 20); do git push -q origin master 2>/dev/null && break; sleep 20; done
for i in $(seq 1 20); do [ "$(git ls-remote origin master 2>/dev/null | cut -f1)" = "$HEAD" ] && break; sleep 15; done
for i in $(seq 1 20); do gh workflow run "$WF" -R nhansp/ios-husky "$@" 2>/dev/null && break; sleep 20; done
R=""
for i in $(seq 1 30); do
	sleep 10
	R=$(gh run list -R nhansp/ios-husky -w "$WF" -L 1 --json databaseId,headSha -q ".[] | select(.headSha==\"$HEAD\") | .databaseId" 2>/dev/null)
	[ -n "$R" ] && break
done
echo "run $R for $HEAD"
[ -n "$R" ] || exit 1
until s=$(gh run view "$R" -R nhansp/ios-husky --json status,conclusion -q '.status+" "+.conclusion' 2>/dev/null) && [[ $s == completed* ]]; do sleep 40; done
echo "$s"
L=$SP/ci-$R.log
if [[ $s != *success ]]; then
	for i in $(seq 1 10); do gh run view "$R" -R nhansp/ios-husky --log-failed > "$L" 2>/dev/null && [ -s "$L" ] && break; sleep 15; done
	grep -E " error: |failed:|Undefined symbols|ld: |FAILED:|##\[error\]" "$L" |
		sed -E 's/.*(MaliMetal|metal-smoke|mesa)\/([^:]+):([0-9]+):[0-9]+: error: /\2:\3: /' | cut -c1-220 | sort -u | head -80
fi