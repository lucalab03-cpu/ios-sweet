#!/bin/sh
# usage: make-fix31.sh BUNDLE.zip PREVIOUS-PAYLOAD.bin [OUTDIR] -> payload-fix31.bin
# fix31 (kernel 0173) is kernel only: a buffer a client closes is handed out again instead of being allocated, wired
# and zeroed; its pages go into the GPU's tables a physical run at a time instead of 4 KB; and the imported pages are
# stored to memory once per group of SUBMITs (boot-arg sweetmalistore=1 brings back a store before every SUBMIT, the
# way it was up to 0172). The dropbox stays as fix30 left it: nothing the shim or /var/sweet does changes this round,
# so no new drop and the phone keeps the sweetdrm-time.log it writes every 10 s.
set -e
B=${1:?usage: make-fix31.sh BUNDLE.zip PREVIOUS-PAYLOAD.bin [OUTDIR]}
OLD=${2:?the payload that boots today (payload-ap.bin)}
OUT=${3:-out}
HERE=$(cd "$(dirname "$0")" && pwd)
IN=$OUT/in-fix31
NEW=$OUT/payload-fix31.bin

count() { grep -a -o "$1" "$2" 2>/dev/null | wc -l | tr -d ' '; }
py=""; for p in python3 python; do $p -c 'import sys' >/dev/null 2>&1 && { py=$p; break; }; done
[ -n "$py" ] || { echo "no python: cannot repayload" >&2; exit 1; }

rm -rf "$IN" && mkdir -p "$IN"
unzip -q -o "$B" -d "$IN"
(cd "$IN" && sha256sum -c SHA256SUMS >/dev/null && echo "bundle sums ok")
grep -E '^xnu ' "$IN/RUNS.txt" || true
K="$IN/kernel/kernel.development.sweet"
if [ ! -f "$K" ]; then
	echo "no kernel in the bundle: nothing to repayload" >&2
	exit 1
fi
# what is in it: 0173's boot-arg, and 0172's chain line (the kernel fix26 and 0172 are in must also be there)
printf 'kernel: %s x "sweetmalistore" (0173), %s x "SWEET MALI: chain" (0172)\n' \
	"$(count sweetmalistore "$K")" "$(count 'SWEET MALI: chain' "$K")"
$py "$HERE/../repayload.py" "$OLD" "$K" "$NEW" --expect "sweetmalistore" | head -2 | cut -c1-140
ls -l "$NEW"
echo "boot: adb shell \"echo 0 > /proc/gpufreqv2/fix_target_opp_index\"; adb shell \"sh /data/local/tmp/go-ios.sh payload-fix31.bin\""
# if the widget page flickers, the imports need a store before every SUBMIT again:
#   python tools/repayload.py PREVIOUS-PAYLOAD.bin KERNEL out/payload-fix31-old.bin --set sweetmalistore=1
