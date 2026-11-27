#!/bin/sh
# usage: make-an.sh BUNDLE.zip -> payload-an/an90/an45.bin (kernel 0171; vsync 60/90/45) + drop-fix15.zip (shim: named BOs once a frame)
set -e
B=$1; IN=/home/claude/in/an; OUT=/home/claude/out
rm -rf $IN && mkdir -p $IN && unzip -q -o "$B" -d $IN
(cd $IN && sha256sum -c SHA256SUMS >/dev/null && echo "bundle sums ok")
cat $IN/RUNS.txt
K=$IN/kernel/kernel.development.sweet
cd /home/claude/ios-sweet/tools
python3 repayload.py $OUT/payload-am.bin $K $OUT/payload-an.bin --expect "0171 vsync every" | head -2 | cut -c1-120
python3 repayload.py $OUT/payload-am.bin $K $OUT/payload-an90.bin --set sweetfb_vsync_div=1 --expect "0171 vsync every" | head -2 | cut -c1-120
python3 repayload.py $OUT/payload-am.bin $K $OUT/payload-an45.bin --set sweetfb_vsync_div=2 --expect "0171 vsync every" | head -2 | cut -c1-120
strings $IN/drop/usr/local/lib/libOSMesa.8.dylib | grep -c "sweet_drm_host_gpu_only" || true
rm -rf $OUT/drop-fix15 $OUT/drop-fix15.zip && mkdir -p $OUT/drop-fix15/var/sweet && cp -R $IN/drop/. $OUT/drop-fix15/
cp $OUT/drop-fix14/var/sweet/outbox.list $OUT/drop-fix15/var/sweet/
printf 'apply\n' > $OUT/drop-fix15/var/sweet/root.cmd
printf 'glwait=0 glblit=0 glstats=1 glcensus=1\n' > $OUT/drop-fix15/var/sweet/glargs
(cd $OUT/drop-fix15 && zip -q -r ../drop-fix15.zip . && find . -type f | sort)
ls -l $OUT/payload-an*.bin $OUT/drop-fix15.zip
