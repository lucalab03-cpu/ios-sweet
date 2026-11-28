#!/bin/sh
# usage: make-fix32.sh BUNDLE.zip [OUTDIR] [PREV-DROP-DIR] -> drop-fix32.zip
# fix32 is a measurement drop, kernel and shim unchanged: glargs adds touch_lat=1 (a "tl: g" line for every frame,
# with when it began and ended) and glslow=20000 (a "tl: slow" line for every GL call that takes 20 ms or more, by
# name). With the kernel calls at 6% of the render thread's time, the frames are p50 7 ms but the page still stalls:
# what is missing is when the time passes between frames and which call takes the 50-100 ms of the tail.
# root.cmd only applies the drop: no profiles this round (sweetstack samples once, it does not say where the time is).
set -e
B=${1:?usage: make-fix32.sh BUNDLE.zip [OUTDIR] [PREV-DROP-DIR]}
OUT=${2:-out}
PREV=$3
HERE=$(cd "$(dirname "$0")" && pwd)
IN=$OUT/in-fix32

count() { grep -a -o "$1" "$2" 2>/dev/null | wc -l | tr -d ' '; }
py=""; for p in python3 python; do $p -c 'import zipfile' >/dev/null 2>&1 && { py=$p; break; }; done
archive() {
	if command -v zip >/dev/null 2>&1; then
		(cd "$1" && zip -q -r "$2" . && find . -type f | sort)
	elif [ -n "$py" ]; then
		(cd "$1" && $py -c 'import os,sys,zipfile
z = zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED)
for d, _, fs in os.walk("."):
	for f in fs:
		p = os.path.join(d, f)
		z.write(p, os.path.relpath(p, os.curdir))
z.close()' "$2" && find . -type f | sort)
	else
		echo "neither zip nor python: cannot make the archive" >&2; return 1
	fi
}

rm -rf "$IN" && mkdir -p "$IN"
unzip -q -o "$B" -d "$IN"
(cd "$IN" && sha256sum -c SHA256SUMS >/dev/null && echo "bundle sums ok")
grep -E '^(xnu|mesa-ios) ' "$IN/RUNS.txt" || true
printf 'shim: %s x touch_lat, %s x "tl: slow", %s x sweetdrm-time.log\n' \
	"$(count 'touch_lat=1' "$IN/drop/usr/local/lib/libOSMesa.8.dylib")" \
	"$(count 'tl: slow' "$IN/drop/usr/local/lib/libOSMesa.8.dylib")" \
	"$(count 'sweetdrm-time\\.log' "$IN/drop/usr/local/lib/libOSMesa.8.dylib")"

rm -rf "$OUT/drop-fix32" "$OUT/drop-fix32.zip"
mkdir -p "$OUT/drop-fix32/var/sweet"
cp -R "$IN/drop/." "$OUT/drop-fix32/"
[ -f "$OUT/drop-fix32/var/sweet/gl" ] || : > "$OUT/drop-fix32/var/sweet/gl"
if [ -n "$PREV" ] && [ -f "$PREV/var/sweet/outbox.list" ]; then
	cp "$PREV/var/sweet/outbox.list" "$OUT/drop-fix32/var/sweet/"
else
	echo "no PREV-DROP-DIR given: no outbox.list (the fetch already carries /var/tmp/sweet*)" >&2
fi
printf 'apply\n' > "$OUT/drop-fix32/var/sweet/root.cmd"
printf 'glwait=0 glblit=0 glstats=1 touch_lat=1 glslow=20000\n' > "$OUT/drop-fix32/var/sweet/glargs"
archive "$OUT/drop-fix32" ../drop-fix32.zip
ls -l "$OUT/drop-fix32.zip"
echo "on the phone: rm -rf drop-fix32 && unzip -o drop-fix32.zip -d drop-fix32 && python3 pack-dropbox.py dropbox.map.json drop-fix32"
