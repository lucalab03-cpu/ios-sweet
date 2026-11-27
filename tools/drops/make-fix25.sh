#!/bin/sh
# usage: make-an.sh BUNDLE.zip -> payload-an/an90/an45.bin (kernel 0171; vsync 60/90/45) + drop-fix25.zip (shim: named BOs once a frame)
set -e
B=$1; IN=/home/claude/in/fix25; OUT=/home/claude/out
rm -rf $IN && mkdir -p $IN && unzip -q -o "$B" -d $IN
(cd $IN && sha256sum -c SHA256SUMS >/dev/null && echo "bundle sums ok")
cat $IN/RUNS.txt
K=$IN/kernel/kernel.development.sweet
cd /home/claude/ios-sweet/tools
strings $IN/drop/usr/local/lib/libOSMesa.8.dylib | grep -c "fix25" || true
rm -rf $OUT/drop-fix25 $OUT/drop-fix25.zip && mkdir -p $OUT/drop-fix25/var/sweet && cp -R $IN/drop/. $OUT/drop-fix25/
cp $OUT/drop-fix24/var/sweet/outbox.list $OUT/drop-fix25/var/sweet/
printf 'apply\n' > $OUT/drop-fix25/var/sweet/root.cmd
printf 'glwait=0 glblit=0 glstats=1 glcensus=1\n' > $OUT/drop-fix25/var/sweet/glargs
(cd $OUT/drop-fix25 && zip -q -r ../drop-fix25.zip . && find . -type f | sort)
ls -l $OUT/drop-fix25.zip
