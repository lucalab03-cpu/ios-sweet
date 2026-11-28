#!/bin/sh
# usage: make-fix30.sh BUNDLE.zip [OUTDIR] [PREV-DROP-DIR] -> drop-fix30.zip + the checks for the round
# fix30 (sweet.drm.c): every SweetMali request timed on CoreAnimation's render thread and on the others, and the
# heaviest stacks of the render thread's waits/SUBMITs of 1 ms or more, every 10 s into /var/tmp/sweetdrm-time.log
# (read it with tools/sweetdrm-time.py). No census and no trace this round: they cost more than they tell here.
# The kernel is unchanged since fix26 (0172), so payload-ap stays: no new payload.
set -e
B=${1:?usage: make-fix30.sh BUNDLE.zip [OUTDIR] [PREV-DROP-DIR]}
OUT=${2:-out}
PREV=$3
IN=$OUT/in-fix30

count() { grep -a -o "$1" "$2" 2>/dev/null | wc -l | tr -d ' '; }   # a marker in a dylib or a kernel image
py=""; for p in python3 python; do $p -c 'import zipfile' >/dev/null 2>&1 && { py=$p; break; }; done   # python3 is a stub on Windows
archive() {                                                        # archive DIR into FILE (no zip everywhere)
	if command -v zip >/dev/null 2>&1; then
		(cd "$1" && zip -q -r "$2" . && find . -type f | sort)
	elif [ -n "$py" ]; then                 # -m zipfile would flatten the tree to file names: walk it ourselves
		(cd "$1" && $py -c 'import os,sys,zipfile
z = zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED)
for d, _, fs in os.walk("."):
	for f in fs:
		p = os.path.join(d, f)
		z.write(p, os.path.relpath(p, os.curdir))
z.close()' "$2" && find . -type f | sort)
	else
		echo "neither zip nor python3: cannot make the archive" >&2; return 1
	fi
}

rm -rf "$IN" && mkdir -p "$IN"
unzip -q -o "$B" -d "$IN"
(cd "$IN" && sha256sum -c SHA256SUMS >/dev/null && echo "bundle sums ok")
grep -E '^(xnu|mesa-ios) ' "$IN/RUNS.txt" || true

# what is in it: the shim's fix30 log line, and 0172 in the kernel (payload-ap already has it)
printf 'shim: %s x sweetdrm-time.log\n' "$(count 'sweetdrm-time\.log' "$IN/drop/usr/local/lib/libOSMesa.8.dylib")"
if [ -f "$IN/kernel/kernel.development.sweet" ]; then
	printf 'kernel: %s x 0172 slow chains\n' "$(count 'SWEET MALI: chain' "$IN/kernel/kernel.development.sweet")"
fi

rm -rf "$OUT/drop-fix30" "$OUT/drop-fix30.zip"
mkdir -p "$OUT/drop-fix30/var/sweet"
cp -R "$IN/drop/." "$OUT/drop-fix30/"
[ -f "$OUT/drop-fix30/var/sweet/gl" ] || : > "$OUT/drop-fix30/var/sweet/gl"
if [ -n "$PREV" ] && [ -f "$PREV/var/sweet/outbox.list" ]; then
	cp "$PREV/var/sweet/outbox.list" "$OUT/drop-fix30/var/sweet/"
else
	echo "no PREV-DROP-DIR given: no outbox.list (the fetch already carries /var/tmp/sweet*)" >&2
fi
# apply the drop, then the two profiles of fix19/20 (the render thread is named, so it can be asked for by name)
printf 'apply\nrun /usr/local/bin/sweetstack prof 90 1500 25 backboardd.orig com.apple.coreanimation.display.primary\nrun /usr/local/bin/sweetstack prof 92 1500 25 SpringBoard #0\n' \
	> "$OUT/drop-fix30/var/sweet/root.cmd"
printf 'glwait=0 glblit=0 glstats=1\n' > "$OUT/drop-fix30/var/sweet/glargs"
archive "$OUT/drop-fix30" ../drop-fix30.zip
ls -l "$OUT/drop-fix30.zip"
echo "on the phone: rm -rf drop-fix30 && unzip -o drop-fix30.zip -d drop-fix30 && python3 pack-dropbox.py dropbox.map.json drop-fix30"
